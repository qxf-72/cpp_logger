#ifndef LOGGER_H
#define LOGGER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>

#include "BlockingQueue.h"

enum class LogLevel { DEBUG = 0, INFO, WARN, ERROR, FATAL };

// 日志器初始化参数。每次成功 init() 都会重置队列峰值和溢出丢弃计数。
struct LoggerConfig {
  static constexpr std::size_t kDefaultMaxFileSize = 1024 * 1024;
  static constexpr std::size_t kDefaultQueueCapacity = 8192;

  std::filesystem::path basePath;
  LogLevel minLevel{LogLevel::DEBUG};
  std::size_t maxFileSize{kDefaultMaxFileSize};
  std::size_t queueCapacity{kDefaultQueueCapacity};
  OverflowPolicy overflowPolicy{OverflowPolicy::Block};
};

class Logger {
 public:
  // 默认单个日志文件上限为 1 MiB。
  static constexpr std::size_t kDefaultMaxFileSize = LoggerConfig::kDefaultMaxFileSize;
  static constexpr std::size_t kDefaultQueueCapacity = LoggerConfig::kDefaultQueueCapacity;

  static Logger& instance();

  // 使用完整配置初始化。queueCapacity 必须大于 0。
  [[nodiscard]] bool init(const LoggerConfig& config);

  // basePath 可包含目录，例如 "logs/app" 会生成 logs/app_日期_序号.log。
  // 保留旧接口；队列使用默认容量和 Block 策略。
  [[nodiscard]] bool init(const std::filesystem::path& basePath,
                          LogLevel minLevel = LogLevel::DEBUG,
                          std::size_t maxFileSize = kDefaultMaxFileSize);
  void setLevel(LogLevel level) noexcept;
  // 宏在构造消息前先调用此函数，避免被过滤的日志产生字符串拼接开销。
  [[nodiscard]] bool shouldLog(LogLevel level) const;
  void log(LogLevel level, const char* file, int line, std::string_view message);
  void stop();

  // 以下统计均为当前初始化周期的数据；DropNewest 与 DropOldest 都计入 droppedCount。
  [[nodiscard]] std::uint64_t droppedCount() const;
  [[nodiscard]] std::size_t queueSize() const;
  [[nodiscard]] std::size_t queuePeakSize() const;

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

 private:
  // 业务线程只构造原始记录；字符串格式化延后至后台线程，缩短日志调用的临界路径。
  struct LogRecord {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::thread::id threadId;
    std::string file;
    int line;
    std::string message;
  };

  static constexpr std::size_t kWriteBatchSize = 64;

  Logger() = default;
  ~Logger();

  // 后台线程负责消费队列、处理滚动并写入文件。
  void workerLoop();
  std::string formatMessage(const LogRecord& record);

  static std::string_view levelToString(LogLevel level) noexcept;
  std::string formatTime(std::chrono::system_clock::time_point timestamp);
  static std::string currentDate();

  std::filesystem::path makeLogFileName() const;
  static std::size_t fileSize(const std::filesystem::path& filename);
  void openNewLogFile();
  void flushPending(std::string& pending);
  void rollIfNeeded(std::string_view message, std::string& pending);

  std::ofstream out_;
  BlockingQueue<LogRecord> queue_;
  std::thread worker_;

  // log() 短暂持共享锁获取当前队列代次；init()/stop() 持独占锁切换生命周期。
  mutable std::shared_mutex lifecycleMutex_;
  std::atomic_int minLevel_{static_cast<int>(LogLevel::DEBUG)};
  std::atomic_bool running_{false};

  std::filesystem::path basePath_;
  std::string currentDate_;
  std::time_t cachedTimestampSecond_{};
  std::string cachedTimestampPrefix_;
  bool hasCachedTimestampPrefix_{false};
  std::size_t currentSize_{0};
  std::size_t maxFileSize_{kDefaultMaxFileSize};
  int fileIndex_{0};
};

#define LOG_IMPL(level, msg)                          \
  do {                                                \
    auto& logger = Logger::instance();                \
    if (logger.shouldLog(level)) {                    \
      logger.log((level), __FILE__, __LINE__, (msg)); \
    }                                                 \
  } while (0)

#define LOG_DEBUG(msg) LOG_IMPL(LogLevel::DEBUG, (msg))
#define LOG_INFO(msg) LOG_IMPL(LogLevel::INFO, (msg))
#define LOG_WARN(msg) LOG_IMPL(LogLevel::WARN, (msg))
#define LOG_ERROR(msg) LOG_IMPL(LogLevel::ERROR, (msg))
#define LOG_FATAL(msg) LOG_IMPL(LogLevel::FATAL, (msg))

#endif
