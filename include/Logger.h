#ifndef LOGGER_H
#define LOGGER_H

#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "BlockingQueue.h"

enum class LogLevel { DEBUG = 0, INFO, WARN, ERROR, FATAL };

class Logger {
 public:
  static Logger& instance();

  bool init(const std::string& filename, LogLevel minLevel = LogLevel::DEBUG);

  void setLevel(LogLevel level);

  void log(LogLevel level, const char* file, int line, const std::string& message);

  void stop();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

 private:
  Logger() = default;
  ~Logger();

  void workerLoop();

  std::string formatMessage(LogLevel level, const char* file, int line, const std::string& message);

  std::string levelToString(LogLevel level) const;
  std::string currentTime() const;

 private:
  std::ofstream out_;

  BlockingQueue<std::string> queue_;
  std::thread worker_;

  std::mutex initMutex_;

  std::atomic_int minLevel_{static_cast<int>(LogLevel::DEBUG)};
  std::atomic_bool running_{false};
};

#define LOG_DEBUG(msg) Logger::instance().log(LogLevel::DEBUG, __FILE__, __LINE__, (msg))
#define LOG_INFO(msg) Logger::instance().log(LogLevel::INFO, __FILE__, __LINE__, (msg))
#define LOG_WARN(msg) Logger::instance().log(LogLevel::WARN, __FILE__, __LINE__, (msg))
#define LOG_ERROR(msg) Logger::instance().log(LogLevel::ERROR, __FILE__, __LINE__, (msg))
#define LOG_FATAL(msg) Logger::instance().log(LogLevel::FATAL, __FILE__, __LINE__, (msg))

#endif
