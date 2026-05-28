#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>   // 显式包含 std::this_thread::get_id，更规范

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
    // 保护初始化过程，避免多个线程同时调用 init()
    std::lock_guard<std::mutex> lock(initMutex_);

    // 如果日志系统已经在运行，直接返回 true
    // 避免重复打开文件、重复创建后台线程
    if (running_.load()) {
        return true;
    }

    // 设置最低日志级别
    // 这里使用 atomic<int>，所以不需要额外加锁
    minLevel_.store(static_cast<int>(minLevel));

    // 以追加模式打开日志文件
    // std::ios::app 表示新日志追加到文件末尾，不覆盖原内容
    out_.open(filename, std::ios::app);

    // 文件打开失败，初始化失败
    if (!out_.is_open()) {
        return false;
    }

    // 标记日志系统已经开始运行
    running_.store(true);

    // 创建后台日志线程
    // 后台线程负责从 BlockingQueue 中取日志，并写入文件
    worker_ = std::thread(&Logger::workerLoop, this);

    return true;
}

// 动态设置日志级别
void Logger::setLevel(LogLevel level) {
    minLevel_.store(static_cast<int>(level));
}

// 写日志接口
// 业务线程调用 LOG_INFO / LOG_WARN 等宏后，最终会进入这个函数
void Logger::log(LogLevel level,
    const char* file,
    int line,
    const std::string& message) {

    // 如果日志系统没有启动，直接丢弃日志
    if (!running_.load()) {
        return;
    }

    // 日志级别过滤
    // 低于当前最低级别的日志不会进入队列
    if (static_cast<int>(level) < minLevel_.load()) {
        return;
    }

    // 先在业务线程中把日志格式化成完整字符串
    // 这样后台线程只负责写文件，不再关心日志格式
    std::string formatted = formatMessage(level, file, line, message);

    // 将日志字符串放入阻塞队列
    // 这里不会直接写文件，所以业务线程可以较快返回
    queue_.push(std::move(formatted));
}

// 停止日志系统
// 主要用于程序退出前安全关闭后台线程和日志文件
void Logger::stop() {
    bool expected = true;

    // compare_exchange_strong 的作用：
    // 只有当 running_ 当前为 true 时，才把它改成 false
    // 这样可以保证 stop() 的核心逻辑只执行一次
    if (running_.compare_exchange_strong(expected, false)) {

        // 关闭阻塞队列
        // close() 会唤醒正在 pop() 中等待的后台线程
        queue_.close();

        // 等待后台日志线程结束
        // 后台线程会先把队列中剩余日志写完，然后退出 workerLoop()
        if (worker_.joinable()) {
            worker_.join();
        }

        // 关闭文件前加锁，避免和 init() 等操作并发冲突
        std::lock_guard<std::mutex> lock(initMutex_);

        // 刷新并关闭日志文件
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

    // 记录已经写入但尚未 flush 的日志条数
    std::size_t writeCount = 0;

    // queue_.pop(message)：
    // 1. 如果队列有数据，取出一条日志并返回 true
    // 2. 如果队列为空，则阻塞等待
    // 3. 如果队列关闭且已经为空，返回 false，循环结束
    while (queue_.pop(message)) {
        // 写入日志文件
        // 使用 '\n' 而不是 std::endl，避免每条日志都强制 flush
        out_ << message << '\n';

        ++writeCount;

        // 每写入 64 条日志，主动 flush 一次
        // 这样比每条日志都 flush 性能更好
        if (writeCount >= 64) {
            out_.flush();
            writeCount = 0;
        }
    }

    // 线程退出前最后 flush 一次
    // 保证剩余未刷新的日志写入文件
    out_.flush();
}

// 格式化一条日志消息
// 把日志级别、时间、线程 ID、文件名、行号和正文拼接成一个字符串
std::string Logger::formatMessage(LogLevel level,
    const char* file,
    int line,
    const std::string& message) {

    std::ostringstream oss;

    oss << "[" << currentTime() << "]"
        << "[" << levelToString(level) << "]"
        << "[tid:" << std::this_thread::get_id() << "]"
        << "[" << file << ":" << line << "] "
        << message;

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
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ) % 1000;

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
    oss << buffer << "."
        << std::setw(3) << std::setfill('0') << milliseconds.count();

    return oss.str();
}