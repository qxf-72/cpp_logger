#include "Logger.h"

#include <charconv>
#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

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

bool isValidOverflowPolicy(OverflowPolicy policy) noexcept {
  switch (policy) {
    case OverflowPolicy::Block:
    case OverflowPolicy::DropNewest:
    case OverflowPolicy::DropOldest:
      return true;
  }
  return false;
}

template <typename Integer>
void appendInteger(std::string& destination, Integer value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc{}) {
    throw std::runtime_error("unable to format integer");
  }
  destination.append(buffer, end);
}
}  // namespace

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

Logger::~Logger() {
  stop();
}

bool Logger::init(const fs::path& basePath, LogLevel minLevel, std::size_t maxFileSize) {
  LoggerConfig config;
  config.basePath = basePath;
  config.minLevel = minLevel;
  config.maxFileSize = maxFileSize;
  return init(config);
}

bool Logger::init(const LoggerConfig& config) {
  // 独占生命周期锁后，已有的 log() 调用会完成，新的调用会等待本次初始化结束。
  std::unique_lock lock(lifecycleMutex_);
  if (running_.load(std::memory_order_acquire) || config.basePath.empty() ||
      config.maxFileSize == 0 || config.queueCapacity == 0 ||
      !isValidOverflowPolicy(config.overflowPolicy)) {
    return false;
  }

  if (worker_.joinable()) {
    worker_.join();
  }

  const fs::path parentPath = config.basePath.parent_path();
  if (!parentPath.empty()) {
    // 允许调用方直接使用尚未创建的日志目录。
    std::error_code error;
    fs::create_directories(parentPath, error);
    if (error) {
      std::cerr << "logger init failed: unable to create directory '" << parentPath.string()
                << "': " << error.message() << '\n';
      return false;
    }
  }

  // stop() 会关闭队列；重新初始化前必须恢复其可写状态并应用新的容量和策略。
  queue_.reset(config.queueCapacity, config.overflowPolicy);
  minLevel_.store(static_cast<int>(config.minLevel), std::memory_order_release);
  basePath_ = config.basePath;
  maxFileSize_ = config.maxFileSize;
  currentDate_ = currentDate();
  hasCachedTimestampPrefix_ = false;
  cachedTimestampPrefix_.clear();
  currentSize_ = 0;
  fileIndex_ = 0;

  try {
    openNewLogFile();
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&Logger::workerLoop, this);
  } catch (const std::exception& error) {
    running_.store(false, std::memory_order_release);
    queue_.close();
    if (out_.is_open()) {
      out_.close();
    }
    std::cerr << "logger init failed: " << error.what() << '\n';
    return false;
  }

  return true;
}

void Logger::setLevel(LogLevel level) noexcept {
  minLevel_.store(static_cast<int>(level), std::memory_order_release);
}

bool Logger::shouldLog(LogLevel level) const {
  // 与 stop() 同步，保证返回 true 后的 log() 不会跨越一次停机操作入队。
  std::shared_lock lock(lifecycleMutex_);
  return running_.load(std::memory_order_acquire) &&
         static_cast<int>(level) >= minLevel_.load(std::memory_order_acquire);
}

void Logger::log(LogLevel level, const char* file, int line, std::string_view message) {
  std::size_t queueGeneration = 0;
  {
    // Block 策略的入队可能等待较长时间，因此不能持有共享生命周期锁等待空位；
    // 否则 stop() 无法取得独占锁并关闭队列来唤醒等待的生产者。
    std::shared_lock lock(lifecycleMutex_);
    if (!running_.load(std::memory_order_acquire) ||
        static_cast<int>(level) < minLevel_.load(std::memory_order_acquire)) {
      return;
    }
    queueGeneration = queue_.generation();
  }

  // 重启后 generation 会改变；旧调用即使在 reset() 后才执行到这里，也会被队列拒绝。
  // 时间和线程 ID 必须由生产者捕获，保证日志反映实际调用时刻；格式化留给后台线程完成。
  LogRecord record{std::chrono::system_clock::now(), level, std::this_thread::get_id(),
                   file == nullptr ? "" : file,      line,  std::string(message)};
  const QueuePushResult result = queue_.push(queueGeneration, std::move(record));
  if (result == QueuePushResult::Closed) {
    return;
  }
}

void Logger::stop() {
  // 关闭队列后，worker 仍会排空已经入队的日志，再由 join() 回收。
  std::unique_lock lock(lifecycleMutex_);
  running_.store(false, std::memory_order_release);
  queue_.close();

  if (worker_.joinable()) {
    worker_.join();
  }

  if (out_.is_open()) {
    out_.flush();
    out_.close();
  }
}

std::uint64_t Logger::droppedCount() const {
  return queue_.droppedCount();
}

std::size_t Logger::queueSize() const {
  return queue_.size();
}

std::size_t Logger::queuePeakSize() const {
  return queue_.peakSize();
}

void Logger::workerLoop() {
  std::vector<LogRecord> batch;
  batch.reserve(kWriteBatchSize);
  std::string pending;

  while (queue_.popBatch(batch, kWriteBatchSize)) {
    try {
      for (const LogRecord& record : batch) {
        const std::string message = formatMessage(record);
        // 只有后台线程会修改文件状态，因此滚动逻辑不需要额外加锁。若需要滚动，
        // rollIfNeeded 会先写入 pending，确保旧文件不会遗失本批已经格式化的内容。
        rollIfNeeded(message, pending);
        pending += message;
        pending += '\n';
        currentSize_ += message.size() + 1;
      }

      // 每批只执行一次 stream::write() 和 flush()，减少逐条 ostream 输出的开销。
      flushPending(pending);
      out_.flush();
      if (!out_) {
        throw std::runtime_error("failed to flush log file");
      }
    } catch (const std::exception& error) {
      std::cerr << "logger worker failed: " << error.what() << '\n';
      running_.store(false, std::memory_order_release);
      queue_.close();
      break;
    }
  }

  if (out_.is_open()) {
    out_.flush();
    if (!out_) {
      std::cerr << "logger worker failed: unable to flush log file\n";
      running_.store(false, std::memory_order_release);
    }
  }
}

std::string Logger::formatMessage(const LogRecord& record) {
  const std::string timestamp = formatTime(record.timestamp);
  const std::string_view level = levelToString(record.level);

  // 提前分配足够空间，避免拼接时间、线程 ID 和文件行号时反复扩容。
  std::string formatted;
  formatted.reserve(timestamp.size() + level.size() + record.file.size() + record.message.size() +
                    64);
  formatted.push_back('[');
  formatted += timestamp;
  formatted += "][";
  formatted += level;
  formatted += "][tid:";
  // std::thread::id 没有数值转换接口，使用稳定的 hash 作为日志中的线程标识。
  appendInteger(formatted, std::hash<std::thread::id>{}(record.threadId));
  formatted += "][";
  formatted += record.file;
  formatted.push_back(':');
  appendInteger(formatted, record.line);
  formatted += "] ";
  formatted += record.message;
  return formatted;
}

std::string_view Logger::levelToString(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::DEBUG:
      return "DEBUG";
    case LogLevel::INFO:
      return "INFO";
    case LogLevel::WARN:
      return "WARN";
    case LogLevel::ERROR:
      return "ERROR";
    case LogLevel::FATAL:
      return "FATAL";
  }
  return "UNKNOWN";
}

std::string Logger::formatTime(std::chrono::system_clock::time_point timestamp) {
  const auto time = std::chrono::system_clock::to_time_t(timestamp);
  if (!hasCachedTimestampPrefix_ || time != cachedTimestampSecond_) {
    const std::tm localTime = toLocalTime(time);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
    cachedTimestampPrefix_ = buffer;
    cachedTimestampSecond_ = time;
    hasCachedTimestampPrefix_ = true;
  }

  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()) % 1000;
  std::string formatted = cachedTimestampPrefix_;
  formatted.push_back('.');
  if (milliseconds.count() < 100) {
    formatted.push_back('0');
  }
  if (milliseconds.count() < 10) {
    formatted.push_back('0');
  }
  appendInteger(formatted, milliseconds.count());
  return formatted;
}

std::string Logger::currentDate() {
  const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  const std::tm localTime = toLocalTime(time);

  char buffer[16];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &localTime);
  return buffer;
}

fs::path Logger::makeLogFileName() const {
  // 保留 basePath 的父目录，只对最后的文件名前缀追加日期和序号。
  const std::string filename = basePath_.filename().string() + '_' + currentDate_ + '_' +
                               std::to_string(fileIndex_) + ".log";
  return basePath_.parent_path() / filename;
}

std::size_t Logger::fileSize(const fs::path& filename) {
  // 文件不存在时 file_size 会通过 error_code 返回错误，此处按新文件的大小 0 处理。
  std::error_code error;
  const auto size = fs::file_size(filename, error);
  return error ? 0 : static_cast<std::size_t>(size);
}

void Logger::openNewLogFile() {
  const fs::path filename = makeLogFileName();
  const std::size_t existingSize = fileSize(filename);
  std::ofstream next(filename, std::ios::out | std::ios::app);
  if (!next.is_open()) {
    throw std::runtime_error("unable to open log file: " + filename.string());
  }

  if (out_.is_open()) {
    // 新文件已成功打开后才关闭旧文件，避免滚动失败时失去当前输出目标。
    out_.flush();
    if (!out_) {
      throw std::runtime_error("unable to flush current log file");
    }
    out_.close();
  }

  out_ = std::move(next);
  currentSize_ = existingSize;
}

void Logger::flushPending(std::string& pending) {
  if (pending.empty()) {
    return;
  }

  out_.write(pending.data(), static_cast<std::streamsize>(pending.size()));
  if (!out_) {
    throw std::runtime_error("failed to write log batch");
  }
  pending.clear();
}

void Logger::rollIfNeeded(std::string_view message, std::string& pending) {
  const std::string today = currentDate();
  if (today != currentDate_) {
    // 日期变更时从新日期的 0 号文件重新开始。
    flushPending(pending);
    currentDate_ = today;
    fileIndex_ = 0;
    openNewLogFile();
  }

  const std::size_t appendSize = message.size() + 1;
  // 单条超大日志允许写入空文件，避免反复滚动却无法写入。
  while (currentSize_ > 0 && currentSize_ + appendSize > maxFileSize_) {
    flushPending(pending);
    ++fileIndex_;
    openNewLogFile();
  }
}
