#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
// 将 time_t 转成本地时间。
// Windows 和 Linux 的线程安全时间转换函数名字不同，这里统一封装，避免业务代码里到处写条件编译。
std::tm toLocalTime(std::time_t value) {
  std::tm tmTime{};
#ifdef _WIN32
  localtime_s(&tmTime, &value);
#else
  localtime_r(&value, &tmTime);
#endif
  return tmTime;
}
}  // 匿名命名空间

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

Logger::~Logger() {
  stop();
}

bool Logger::init(const std::string& baseName, LogLevel minlevel, std::size_t maxFileSize) {
  std::lock_guard<std::mutex> lock(initMutex_);
  if (running_.load()) {
    return true;
  }

  if (worker_.joinable()) {
    // 上一次 stop 后线程对象可能仍处于 joinable 状态，重新初始化前必须先回收它。
    worker_.join();
  }

  if (baseName.empty() || maxFileSize == 0) {
    return false;
  }

  // stop() 会关闭队列；如果允许再次 init，就必须把队列恢复到可写状态。
  queue_.reset();
  minLevel_.store(static_cast<int>(minlevel));
  baseName_ = baseName;
  maxFileSize_ = maxFileSize;
  currentDate_ = currentDate();
  currentSize_ = 0;
  fileIndex_ = 0;

  try {
    openNewLogFile();
  } catch (const std::exception& e) {
    // 文件打开失败时不要启动后台线程，否则后续日志会进入一个没有输出目标的 worker。
    std::cerr << "logger init failed: " << e.what() << std::endl;
    queue_.close();
    return false;
  }

  running_.store(true);
  worker_ = std::thread([this]() { workerLoop(); });

  return true;
}

void Logger::setLevel(LogLevel level) {
  minLevel_ = static_cast<int>(level);
}

bool Logger::shouldLog(LogLevel level) const {
  return running_.load() && static_cast<int>(level) >= minLevel_.load();
}

void Logger::log(LogLevel level, const char* file, int line, const std::string& message) {
  // 防御性检查
  if (!shouldLog(level)) {
    return;
  }

  std::string msg = formatMessage(level, file, line, message);
  if (!queue_.push(std::move(msg))) {
    running_.store(false);
  }
}

void Logger::stop() {
  std::lock_guard<std::mutex> lock(initMutex_);

  // 先标记停止，再关闭队列唤醒 worker；worker 会继续写完队列里已经存在的日志。
  running_.store(false);
  queue_.close();

  if (worker_.joinable()) {
    worker_.join();
  }

  if (out_.is_open()) {
    out_.flush();
    out_.close();
  }
}

void Logger::workerLoop() {
  std::string message;
  std::size_t writeCount = 0;

  while (queue_.pop(message)) {
    try {
      // 每条日志写入前都检查是否需要按日期或文件大小切换输出文件。
      rollIfNeeded(message);

      out_ << message << '\n';
      if (!out_) {
        throw std::runtime_error("write log fail.");
      }

      currentSize_ += message.size() + 1;

      ++writeCount;
      if (writeCount >= 64) {
        // 批量刷新可以减少磁盘 flush 次数，同时避免长时间不落盘。
        out_.flush();
        writeCount = 0;
      }
    } catch (const std::exception& e) {
      // 后台线程不能让异常逃逸，否则 std::thread 会触发 std::terminate 直接结束进程。
      std::cerr << "logger worker failed: " << e.what() << std::endl;
      running_.store(false);
      queue_.close();
      break;
    }
  }

  if (out_.is_open()) {
    out_.flush();
  }
}

std::string Logger::formatMessage(LogLevel level, const char* file, int line,
                                  const std::string& message) {
  std::ostringstream oss;

  // 输出格式：[时间][级别][线程ID][源文件:行号] 日志正文。
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
  // 精确到毫秒，方便排查并发场景下的日志先后顺序。
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::tm tmTime = toLocalTime(time);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmTime);

  std::ostringstream oss;
  oss << buffer << "." << std::setw(3) << std::setfill('0') << milliseconds.count();

  return oss.str();
}

std::string Logger::currentDate() const {
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tmTime = toLocalTime(time);

  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tmTime);

  return buffer;
}

std::string Logger::makeLogFileName() const {
  std::ostringstream oss;
  oss << baseName_ << '_' << currentDate_ << '_' << fileIndex_ << ".log";
  return oss.str();
}

std::size_t Logger::fileSize(const std::string& filename) const {
  // 以 ate 方式打开后读 tellg，可以得到已有日志文件的字节数。
  // 如果文件不存在，说明这是新文件，大小按 0 处理。
  std::ifstream in(filename.c_str(), std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    return 0;
  }

  std::ifstream::pos_type pos = in.tellg();
  if (pos == std::ifstream::pos_type(-1)) {
    return 0;
  }

  return static_cast<std::size_t>(pos);
}

void Logger::openNewLogFile() {
  const std::string filename = makeLogFileName();
  const std::size_t existingSize = fileSize(filename);

  // 先打开新文件，成功后再替换当前 out_。
  // 这样滚动失败时不会提前关闭旧文件，避免日志输出目标丢失。
  std::ofstream next(filename.c_str(), std::ios::out | std::ios::app);
  if (!next.is_open()) {
    throw std::runtime_error("open file fail: " + filename);
  }

  if (out_.is_open()) {
    out_.flush();
    out_.close();
  }

  out_ = std::move(next);
  currentSize_ = existingSize;
}

void Logger::rollIfNeeded(const std::string& message) {
  std::string today = currentDate();

  if (today != currentDate_) {
    // 日期变化时从新日期的 0 号文件重新开始写。
    currentDate_ = today;
    fileIndex_ = 0;
    openNewLogFile();
  }

  const std::size_t appendSize = message.size() + 1;
  // 当前文件已有内容且追加后会超过上限时，继续寻找下一个可写文件。
  // 如果单条日志本身就超过上限，则允许它写入空文件，避免陷入无限滚动。
  while (currentSize_ > 0 && currentSize_ + appendSize > maxFileSize_) {
    ++fileIndex_;
    openNewLogFile();
  }
}
