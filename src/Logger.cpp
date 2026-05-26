#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    stop();
}

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

void Logger::setLevel(LogLevel level) {
    minLevel_.store(static_cast<int>(level));
}

void Logger::log(LogLevel level,
    const char* file,
    int line,
    const std::string& message) {
    if (!running_.load()) {
        return;
    }

    if (static_cast<int>(level) < minLevel_.load()) {
        return;
    }

    std::string formatted = formatMessage(level, file, line, message);

    queue_.push(std::move(formatted));
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

std::string Logger::currentTime() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ) % 1000;

    std::tm tmTime{};
    localtime_r(&time, &tmTime);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmTime);

    std::ostringstream oss;
    oss << buffer << "."
        << std::setw(3) << std::setfill('0') << milliseconds.count();

    return oss.str();
}