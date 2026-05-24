#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <mutex>

// 日志等级。数值越大，等级越高。
enum class LogLevel {
    DEBUG = 0,
    INFO,
    WARN,
    ERROR
};

class Logger {
public:
    // 获取全局唯一的 Logger 实例。
    static Logger& instance();

    // 初始化日志文件。
    // 返回 true 表示日志文件打开成功，false 表示初始化失败。
    bool init(const std::string& filename, LogLevel minLevel = LogLevel::DEBUG);

    // 设置最低输出日志等级。
    // 低于该等级的日志会被忽略。
    void setLevel(LogLevel level);

    // 写入一条日志。
    // file 和 line 通常由 LOG_xxx 宏自动传入。
    void log(LogLevel level, const char* file, int line, const std::string& message);

    // 禁止拷贝和赋值，避免产生多个 Logger 实例。
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

private:
    Logger() = default;
    ~Logger();

    std::string levelToString(LogLevel level);
    std::string currentTime();

private:
    std::ofstream out_;                   // 日志文件输出流
    std::mutex mutex_;                    // 保护日志状态，保证多线程写入安全
    LogLevel minLevel_ = LogLevel::DEBUG; // 最低输出等级
    bool initialized_ = false;            // 是否已成功初始化
};

// 使用宏捕获调用处的文件名和行号。
// 不能简单改成普通函数!!! 否则 __FILE__ 和 __LINE__ 会变成函数定义处的位置。
#define LOG_DEBUG(msg) Logger::instance().log(LogLevel::DEBUG, __FILE__, __LINE__, (msg))
#define LOG_INFO(msg) Logger::instance().log(LogLevel::INFO, __FILE__, __LINE__, (msg))
#define LOG_WARN(msg) Logger::instance().log(LogLevel::WARN, __FILE__, __LINE__, (msg))
#define LOG_ERROR(msg) Logger::instance().log(LogLevel::ERROR, __FILE__, __LINE__, (msg))

#endif