#ifndef LOGGER_H
#define LOGGER_H

#include <atomic>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "BlockingQueue.h"

enum class LogLevel { DEBUG = 0, INFO, WARN, ERROR, FATAL };

class Logger {
 public:
  static Logger& instance();

  // 初始化日志器。
  // baseName 是日志文件名前缀，最终文件名形如 baseName_2026-06-25_0.log。
  // maxFileSize 控制单个日志文件的最大字节数，到达上限后会递增 fileIndex_ 滚动到新文件。
  bool init(const std::string& baseName, LogLevel minLevel = LogLevel::DEBUG,
            std::size_t maxFileSize = 1 * 1024 * 1024);

  void setLevel(LogLevel level);

  // 供宏在构造日志内容之前做快速过滤，避免低等级日志仍执行字符串拼接等开销。
  bool shouldLog(LogLevel level) const;

  void log(LogLevel level, const char* file, int line, const std::string& message);

  // 主动停止后台线程。
  // stop 会关闭队列、等待剩余日志写入完成，并刷新/关闭当前日志文件。
  void stop();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

 private:
  Logger() = default;
  ~Logger();

  // 后台消费线程主循环：从阻塞队列取日志，处理文件滚动，并写入文件。
  void workerLoop();

  std::string formatMessage(LogLevel level, const char* file, int line, const std::string& message);

  std::string levelToString(LogLevel level) const;
  std::string currentTime() const;

  // 以下函数只在后台线程或初始化阶段使用，用来维护按日期和大小滚动的日志文件。
  std::string currentDate() const;
  std::string makeLogFileName() const;
  std::size_t fileSize(const std::string& filename) const;
  void openNewLogFile();
  void rollIfNeeded(const std::string& message);

 private:
  std::ofstream out_;

  BlockingQueue<std::string> queue_;
  std::thread worker_;

  std::mutex initMutex_;

  std::atomic_int minLevel_{static_cast<int>(LogLevel::DEBUG)};
  std::atomic_bool running_{false};

  std::string baseName_;  // 日志文件名前缀，例如传入 "app" 会生成 app_日期_序号.log。
  std::string currentDate_;  // 当前打开文件对应的日期，日期变化时会切换到新一天的 0 号文件。
  std::size_t currentSize_{0};  // 当前日志文件已经写入的字节数，用于判断是否需要按大小滚动。
  std::size_t maxFileSize_{1 * 1024 * 1024};  // 单个日志文件允许写入的最大字节数。
  int fileIndex_{0};  // 当天日志文件序号，文件超过大小上限后递增。
};

// 宏里先调用 shouldLog，再求值 msg。
// 这样被过滤掉的日志不会触发字符串拼接、函数调用等额外开销。
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
