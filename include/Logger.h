#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <mutex>

enum class LogLevel
{
    DEBUG = 0,
    INFO,
    WARN,
    ERROR
};

class Logger
{
public:
    static Logger &instance();

    bool init(const std::string &filename, LogLevel minLevel = LogLevel::DEBUG);

    void setLevel(LogLevel level);

    void log(LogLevel level,
             const char *file,
             int line,
             const std::string &message);

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

private:
    Logger() = default;
    ~Logger();

    std::string levelToString(LogLevel level);
    std::string currentTime();

private:
    std::ofstream out_;
    std::mutex mutex_;
    LogLevel minLevel_ = LogLevel::DEBUG;
    bool initialized_ = false;
};

#define LOG_DEBUG(msg) Logger::instance().log(LogLevel::DEBUG, __FILE__, __LINE__, (msg))
#define LOG_INFO(msg) Logger::instance().log(LogLevel::INFO, __FILE__, __LINE__, (msg))
#define LOG_WARN(msg) Logger::instance().log(LogLevel::WARN, __FILE__, __LINE__, (msg))
#define LOG_ERROR(msg) Logger::instance().log(LogLevel::ERROR, __FILE__, __LINE__, (msg))

#endif