#include "Logger.h"

#include <charconv>
#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <stdexcept>
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

bool isValidLogLevel(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::DEBUG:
    case LogLevel::INFO:
    case LogLevel::WARN:
    case LogLevel::ERROR:
    case LogLevel::FATAL:
      return true;
  }
  return false;
}

bool isValidFlushPolicy(FlushPolicy policy) noexcept {
  switch (policy) {
    case FlushPolicy::OnStop:
    case FlushPolicy::Periodic:
    case FlushPolicy::EveryBatch:
      return true;
  }
  return false;
}

bool isValidConsoleStream(ConsoleStream stream) noexcept {
  switch (stream) {
    case ConsoleStream::Stdout:
    case ConsoleStream::Stderr:
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
  if (running_.load(std::memory_order_acquire) || config.queueCapacity == 0 ||
      config.writeBatchSize == 0 || !isValidLogLevel(config.minLevel) ||
      !isValidOverflowPolicy(config.overflowPolicy) || !isValidFlushPolicy(config.flushPolicy) ||
      (config.flushPolicy == FlushPolicy::Periodic && config.flushInterval.count() <= 0) ||
      (config.flushAtOrAbove.has_value() && !isValidLogLevel(*config.flushAtOrAbove)) ||
      !isValidConsoleStream(config.consoleStream) ||
      (!config.enableFileSink && !config.enableConsoleSink && config.additionalSinks.empty()) ||
      (config.enableFileSink && (config.basePath.empty() || config.maxFileSize == 0))) {
    return false;
  }

  for (const std::shared_ptr<LogSink>& sink : config.additionalSinks) {
    if (!sink) {
      return false;
    }
  }

  if (worker_.joinable()) {
    worker_.join();
  }

  closeSinks();
  sinks_.clear();

  std::vector<std::shared_ptr<LogSink>> newSinks;
  try {
    if (config.enableFileSink) {
      newSinks.push_back(
          detail::makeFileSink(config.basePath, config.maxFileSize, config.writeBatchSize));
    }
    if (config.enableConsoleSink) {
      newSinks.push_back(detail::makeConsoleSink(config.consoleStream, config.writeBatchSize));
    }
    newSinks.insert(newSinks.end(), config.additionalSinks.begin(), config.additionalSinks.end());
  } catch (const std::exception& error) {
    std::cerr << "logger init failed: " << error.what() << '\n';
    return false;
  }

  // stop() 会关闭队列；重新初始化前必须恢复其可写状态并应用新的容量和策略。
  queue_.reset(config.queueCapacity, config.overflowPolicy);
  minLevel_.store(static_cast<int>(config.minLevel), std::memory_order_release);
  writeBatchSize_ = config.writeBatchSize;
  flushPolicy_ = config.flushPolicy;
  flushInterval_ = config.flushInterval;
  flushAtOrAbove_ = config.flushAtOrAbove;
  hasCachedTimestampPrefix_ = false;
  cachedTimestampPrefix_.clear();
  sinks_ = std::move(newSinks);

  try {
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&Logger::workerLoop, this);
  } catch (const std::exception& error) {
    running_.store(false, std::memory_order_release);
    queue_.close();
    closeSinks();
    sinks_.clear();
    std::cerr << "logger init failed: " << error.what() << '\n';
    return false;
  }

  return true;
}

void Logger::setLevel(LogLevel level) noexcept {
  minLevel_.store(static_cast<int>(level), std::memory_order_release);
}

bool Logger::shouldLog(LogLevel level) const {
  // log() 会在入队前以生命周期锁和队列代次复核。这里仅作为宏的无锁快筛，
  // 避免通常路径为同一条日志获取两次共享锁。
  return running_.load(std::memory_order_acquire) &&
         static_cast<int>(level) >= minLevel_.load(std::memory_order_acquire);
}

void Logger::log(LogLevel level, const char* file, int line, std::string_view message) {
  logImpl(level, file, line, message, false);
}

void Logger::logStatic(LogLevel level, const char* file, int line, std::string_view message) {
  logImpl(level, file, line, message, true);
}

void Logger::logImpl(LogLevel level, const char* file, int line, std::string_view message,
                     bool fileHasStaticStorage) {
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
  LogRecord record{std::chrono::system_clock::now(),
                   level,
                   std::this_thread::get_id(),
                   fileHasStaticStorage ? (file == nullptr ? "" : file) : nullptr,
                   fileHasStaticStorage ? std::string{} : std::string(file == nullptr ? "" : file),
                   line,
                   std::string(message)};
  static_cast<void>(queue_.push(queueGeneration, std::move(record)));
}

void Logger::stop() {
  // 关闭队列后，worker 仍会排空已经入队的日志，再由 join() 回收。
  std::unique_lock lock(lifecycleMutex_);
  running_.store(false, std::memory_order_release);
  queue_.close();

  if (worker_.joinable()) {
    worker_.join();
  }

  closeSinks();
  sinks_.clear();
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
  try {
    std::vector<LogRecord> batch;
    batch.reserve(writeBatchSize_);
    LogSink::Batch formattedBatch;
    formattedBatch.reserve(writeBatchSize_);
    auto nextFlush = std::chrono::steady_clock::now() + flushInterval_;

    while (true) {
      bool hasBatch = false;
      if (flushPolicy_ == FlushPolicy::Periodic) {
        const auto now = std::chrono::steady_clock::now();
        const auto timeout =
            now >= nextFlush
                ? std::chrono::milliseconds::zero()
                : std::chrono::duration_cast<std::chrono::milliseconds>(nextFlush - now);
        hasBatch = queue_.popBatchFor(batch, writeBatchSize_, timeout);
      } else {
        hasBatch = queue_.popBatch(batch, writeBatchSize_);
      }

      if (!hasBatch) {
        if (flushPolicy_ == FlushPolicy::Periodic &&
            std::chrono::steady_clock::now() >= nextFlush) {
          flushSinks();
          nextFlush = std::chrono::steady_clock::now() + flushInterval_;
        }
        if (queue_.closed()) {
          break;
        }
        continue;
      }

      bool flushForLevel = false;
      formattedBatch.clear();
      for (const LogRecord& record : batch) {
        formattedBatch.push_back(formatMessage(record));
        flushForLevel =
            flushForLevel || (flushAtOrAbove_.has_value() &&
                              static_cast<int>(record.level) >= static_cast<int>(*flushAtOrAbove_));
      }

      // 每个 Sink 都接收同一批格式化记录；FileSink 内部仍按记录判断滚动边界。
      for (const std::shared_ptr<LogSink>& sink : sinks_) {
        sink->writeBatch(formattedBatch);
      }

      const bool periodicFlush =
          flushPolicy_ == FlushPolicy::Periodic && std::chrono::steady_clock::now() >= nextFlush;
      if (flushPolicy_ == FlushPolicy::EveryBatch || flushForLevel || periodicFlush) {
        flushSinks();
        if (flushPolicy_ == FlushPolicy::Periodic) {
          nextFlush = std::chrono::steady_clock::now() + flushInterval_;
        }
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "logger worker failed: " << error.what() << '\n';
    running_.store(false, std::memory_order_release);
    queue_.close();
  }

  try {
    // 无论是正常排空还是异常退出，都尽力刷新已经成功交给 Sink 的记录。
    flushSinks();
  } catch (const std::exception& error) {
    std::cerr << "logger worker failed while flushing sinks: " << error.what() << '\n';
    running_.store(false, std::memory_order_release);
    queue_.close();
  }
}

void Logger::flushSinks() {
  for (const std::shared_ptr<LogSink>& sink : sinks_) {
    sink->flush();
  }
}

void Logger::closeSinks() noexcept {
  for (const std::shared_ptr<LogSink>& sink : sinks_) {
    if (sink) {
      sink->close();
    }
  }
}

std::string Logger::formatMessage(const LogRecord& record) {
  const std::string timestamp = formatTime(record.timestamp);
  const std::string_view level = levelToString(record.level);
  const std::string_view file = record.staticFile == nullptr ? std::string_view(record.ownedFile)
                                                             : std::string_view(record.staticFile);

  // 提前分配足够空间，避免拼接时间、线程 ID 和文件行号时反复扩容。
  std::string formatted;
  formatted.reserve(timestamp.size() + level.size() + file.size() + record.message.size() + 64);
  formatted.push_back('[');
  formatted += timestamp;
  formatted += "][";
  formatted += level;
  formatted += "][tid:";
  // std::thread::id 没有数值转换接口，使用稳定的 hash 作为日志中的线程标识。
  appendInteger(formatted, std::hash<std::thread::id>{}(record.threadId));
  formatted += "][";
  formatted += file;
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
