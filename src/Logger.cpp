#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

// 获取全局唯一的 Logger 实例
// 函数内 static 局部变量在 C++11 之后是线程安全初始化的
Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

// 析构函数中调用 stop()
// 目的是保证程序退出时，后台日志线程能够正常结束，剩余日志能够写入文件
Logger::~Logger() {
  stop();
}

// 初始化日志系统
// filename: 日志文件名
// minLevel: 最低输出日志级别
bool Logger::init(const std::string& filename, LogLevel minLevel) {
  std::lock_guard<std::mutex> lock(initMutex_);

  if (running_.load()) {
    return true;
  }

  minLevel_.store(static_cast<int>(minLevel));

  out_.open(filename, std::ios::app);
  if (!out_.is_open()) {
    return false;
  }

  running_.store(true);
  worker_ = std::thread(&Logger::workerLoop, this);

  return true;
}

// 动态设置日志级别
void Logger::setLevel(LogLevel level) {
  minLevel_.store(static_cast<int>(level));
}

// 写日志接口
// 业务线程调用 LOG_INFO / LOG_WARN 等宏后，最终会进入这个函数
void Logger::log(LogLevel level, const char* file, int line, const std::string& message) {
  if (!running_.load()) {
    return;
  }
  if (static_cast<int>(level) < minLevel_.load()) {
    return;
  }
  std::string formatted = formatMessage(level, file, line, message);
  queue_.push(std::move(formatted));
}

// 停止日志系统
// 主要用于程序退出前安全关闭后台线程和日志文件
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

// 后台日志线程执行的函数
// 它不断从 BlockingQueue 中取日志，然后写入文件
void Logger::workerLoop() {
  std::string message;

  std::size_t writeCount = 0;

  // queue_.pop(message)：
  // 1. 如果队列有数据，取出一条日志并返回 true
  // 2. 如果队列为空，则阻塞等待
  // 3. 如果队列关闭且已经为空，返回 false，循环结束
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

// 格式化一条日志消息
// 把日志级别、时间、线程 ID、文件名、行号和正文拼接成一个字符串
std::string Logger::formatMessage(LogLevel level, const char* file, int line,
                                  const std::string& message) {
  std::ostringstream oss;

  oss << "[" << currentTime() << "]"
      << "[" << levelToString(level) << "]"
      << "[tid:" << std::this_thread::get_id() << "]"
      << "[" << file << ":" << line << "] " << message;

  return oss.str();
}

// 将日志级别枚举转换成字符串
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
    default:
      return "UNKNOWN";
  }
}

// 获取当前时间字符串
// 格式示例：2026-05-25 12:00:00.123
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
