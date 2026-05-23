#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>

Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}

Logger::~Logger()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (out_.is_open())
    {
        out_.close();
    }
}

bool Logger::init(const std::string &filename, LogLevel minLevel)
{
    std::lock_guard<std::mutex> lock(mutex_);

    minLevel_ = minLevel;
    out_.open(filename, std::ios::app);

    initialized_ = out_.is_open();
    return initialized_;
}

void Logger::setLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(mutex_);
    minLevel_ = level;
}

void Logger::log(LogLevel level,
                 const char *file,
                 int line,
                 const std::string &message)
{
    if (static_cast<int>(level) < static_cast<int>(minLevel_))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_)
    {
        return;
    }

    out_ << "[" << currentTime() << "]"
         << "[" << levelToString(level) << "]"
         << "[tid:" << std::this_thread::get_id() << "]"
         << "[" << file << ":" << line << "] "
         << message
         << std::endl;
}

std::string Logger::levelToString(LogLevel level)
{
    switch (level)
    {
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

std::string Logger::currentTime()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    std::tm tm_time{};
    localtime_r(&time, &tm_time);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_time);

    return std::string(buffer);
}