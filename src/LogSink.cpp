#include "LogSink.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
namespace fs = std::filesystem;

// Windows 与 POSIX 的线程安全本地时间转换接口不同，统一封装在这里。
std::tm toLocalTime(std::time_t value) {
  std::tm localTime{};
#ifdef _WIN32
  localtime_s(&localTime, &value);
#else
  localtime_r(&value, &localTime);
#endif
  return localTime;
}

std::string currentDate() {
  const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  const std::tm localTime = toLocalTime(time);

  char buffer[16];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &localTime);
  return buffer;
}

class FileSink final : public LogSink {
 public:
  FileSink(fs::path basePath, std::size_t maxFileSize, std::size_t writeBatchSize)
      : basePath_(std::move(basePath)), currentDate_(currentDate()), maxFileSize_(maxFileSize) {
    if (basePath_.empty() || maxFileSize_ == 0) {
      throw std::invalid_argument("file sink requires a non-empty path and positive file size");
    }

    const fs::path parentPath = basePath_.parent_path();
    if (!parentPath.empty()) {
      std::error_code error;
      fs::create_directories(parentPath, error);
      if (error) {
        throw std::runtime_error("unable to create log directory '" + parentPath.string() +
                                 "': " + error.message());
      }
    }

    reservePending(writeBatchSize);
    openNewLogFile();
  }

  void writeBatch(const Batch& records) override {
    for (const std::string& record : records) {
      // 若本条记录需要滚动，先将当前文件所属的已格式化内容写完。
      rollIfNeeded(record);
      pending_ += record;
      pending_ += '\n';
      currentSize_ += record.size() + 1;
    }
    flushPending();
  }

  void flush() override {
    if (!out_.is_open()) {
      return;
    }
    out_.flush();
    if (!out_) {
      throw std::runtime_error("failed to flush log file");
    }
  }

  void close() noexcept override {
    try {
      if (out_.is_open()) {
        out_.flush();
        out_.close();
      }
    } catch (...) {
      // stop() 不能因析构或关闭阶段的 I/O 异常而抛出。
    }
  }

 private:
  fs::path makeLogFileName() const {
    const std::string filename = basePath_.filename().string() + '_' + currentDate_ + '_' +
                                 std::to_string(fileIndex_) + ".log";
    return basePath_.parent_path() / filename;
  }

  static std::size_t fileSize(const fs::path& filename) {
    std::error_code error;
    const auto size = fs::file_size(filename, error);
    return error ? 0 : static_cast<std::size_t>(size);
  }

  void openNewLogFile() {
    const fs::path filename = makeLogFileName();
    const std::size_t existingSize = fileSize(filename);
    // 二进制模式保证 '\n' 始终只占一个字节，让 currentSize_ 与文件实际大小一致。
    std::ofstream next(filename, std::ios::out | std::ios::app | std::ios::binary);
    if (!next.is_open()) {
      throw std::runtime_error("unable to open log file: " + filename.string());
    }

    if (out_.is_open()) {
      out_.flush();
      if (!out_) {
        throw std::runtime_error("unable to flush current log file");
      }
      out_.close();
    }

    out_ = std::move(next);
    currentSize_ = existingSize;
  }

  void flushPending() {
    if (pending_.empty()) {
      return;
    }

    out_.write(pending_.data(), static_cast<std::streamsize>(pending_.size()));
    if (!out_) {
      throw std::runtime_error("failed to write log batch");
    }
    pending_.clear();
  }

  void rollIfNeeded(std::string_view record) {
    const std::string today = currentDate();
    if (today != currentDate_) {
      // 日期变更时从新日期的 0 号文件重新开始。
      flushPending();
      currentDate_ = today;
      fileIndex_ = 0;
      openNewLogFile();
    }

    const std::size_t appendSize = record.size() + 1;
    // 单条超大日志允许写入空文件，避免反复滚动却无法写入。
    while (currentSize_ > 0 &&
           (appendSize > maxFileSize_ || currentSize_ > maxFileSize_ - appendSize)) {
      flushPending();
      ++fileIndex_;
      openNewLogFile();
    }
  }

  void reservePending(std::size_t writeBatchSize) {
    constexpr std::size_t kEstimatedFormattedRecordSize = 256;
    if (writeBatchSize <= std::numeric_limits<std::size_t>::max() / kEstimatedFormattedRecordSize) {
      pending_.reserve(writeBatchSize * kEstimatedFormattedRecordSize);
    }
  }

  fs::path basePath_;
  std::ofstream out_;
  std::string currentDate_;
  std::string pending_;
  std::size_t currentSize_{0};
  std::size_t maxFileSize_{0};
  int fileIndex_{0};
};

class ConsoleSink final : public LogSink {
 public:
  ConsoleSink(ConsoleStream stream, std::size_t writeBatchSize) : stream_(stream) {
    reservePending(writeBatchSize);
  }

  void writeBatch(const Batch& records) override {
    for (const std::string& record : records) {
      pending_ += record;
      pending_ += '\n';
    }

    if (pending_.empty()) {
      return;
    }
    std::ostream& output = outputStream();
    output.write(pending_.data(), static_cast<std::streamsize>(pending_.size()));
    if (!output) {
      throw std::runtime_error("failed to write console log batch");
    }
    pending_.clear();
  }

  void flush() override {
    std::ostream& output = outputStream();
    output.flush();
    if (!output) {
      throw std::runtime_error("failed to flush console output");
    }
  }

  void close() noexcept override {
    try {
      outputStream().flush();
    } catch (...) {
      // 控制台没有独占资源，关闭时尽力刷新即可。
    }
  }

 private:
  std::ostream& outputStream() const {
    return stream_ == ConsoleStream::Stderr ? std::cerr : std::cout;
  }

  void reservePending(std::size_t writeBatchSize) {
    constexpr std::size_t kEstimatedFormattedRecordSize = 256;
    if (writeBatchSize <= std::numeric_limits<std::size_t>::max() / kEstimatedFormattedRecordSize) {
      pending_.reserve(writeBatchSize * kEstimatedFormattedRecordSize);
    }
  }

  ConsoleStream stream_;
  std::string pending_;
};
}  // namespace

namespace detail {

std::shared_ptr<LogSink> makeFileSink(const std::filesystem::path& basePath,
                                      std::size_t maxFileSize, std::size_t writeBatchSize) {
  return std::make_shared<FileSink>(basePath, maxFileSize, writeBatchSize);
}

std::shared_ptr<LogSink> makeConsoleSink(ConsoleStream stream, std::size_t writeBatchSize) {
  return std::make_shared<ConsoleSink>(stream, writeBatchSize);
}

}  // namespace detail
