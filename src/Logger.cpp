#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

Logger::~Logger() {
  stop();
}

bool Logger::init(const std::string& filename, LogLevel minlevel) {
  std::lock_guard<std::mutex> lock(initMutex_);
  if (running_.load()) {
    return true;
  }
  minLevel_.store(static_cast<int>(minlevel));

  out_.open(filename, std::ios::app);
  if (out_.is_open() == false) {
    return false;
  }

  running_.store(true);
  worker_ = std::thread([this]() { workerLoop(); });

  return true;
}

void Logger::setLevel(LogLevel level) {
  minLevel_ = static_cast<int>(level);
}

void Logger::log(LogLevel level, const char* file, int line, const std::string& message) {
  if (running_.load() == false) {
    return;
  }
  if (static_cast<int>(level) < minLevel_.load()) {
    return;
  }
  std::string msg = formatMessage(level, file, line, message);
  queue_.push(std::move(msg));
}

void Logger::stop() {
  bool expected = true;
  if (running_.compare_exchange_strong(expected, false)) {
    queue_.close();
    if (worker_.joinable()) {
      worker_.join();
    }
    std::lock_guard<std::mutex> lock(initMutex_);
    if (out_.is_open()) {
      out_.flush();
      out_.close();
    }
  }
}

void Logger::workerLoop() {
  std::string message;
  std::size_t writeCount = 0;
  while (queue_.pop(message)) {
    out_ << message << '\n';

    ++writeCount;
    if (writeCount >= 64) {
      out_.flush();
      writeCount = 0;
    }
  }

  out_.flush();
}

std::string Logger::formatMessage(LogLevel level, const char* file, int line,
                                  const std::string& message) {
  std::ostringstream oss;

  oss << "[" << currentTime() << "]"
      << "[" << levelToString(level) << "]"
      << "[tid:" << std::this_thread::get_id() << "]"
      << "[" << file << ":" << line << "] " << message;

  return oss.str();
}

std::string Logger::levelToString(LogLevel level) const {
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
    default:
      return "UNKNOWN";
  }
}

std::string Logger::currentTime() const {
  // 获取当前系统时间点
  auto now = std::chrono::system_clock::now();
  // 转换成 time_t，方便使用 C 风格时间函数处理年月日时分秒
  auto time = std::chrono::system_clock::to_time_t(now);
  // 取出当前时间点中的毫秒部分
  auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  // tm 结构体用于保存本地时间的年月日时分秒
  std::tm tmTime{};
  // localtime_r 是线程安全版本
  // 不建议在多线程程序中使用 localtime()
  localtime_r(&time, &tmTime);
  // 格式化年月日时分秒
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmTime);
  // 拼接毫秒部分
  std::ostringstream oss;
  oss << buffer << "." << std::setw(3) << std::setfill('0') << milliseconds.count();

  return oss.str();
}
