#include "Logger.h"
#include <iostream>
#include <string>
#include <thread>
#include <vector>

class ThreadGuard {
    std::thread t;  // 持有线程对象，而不是引用

public:
    // 接受右值引用，支持移动语义
    explicit ThreadGuard(std::thread t_) : t(std::move(t_)) {}

    ~ThreadGuard() {
        if (t.joinable()) {
            t.join();
        }
    }

    ThreadGuard(const ThreadGuard&) = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;

    // 支持移动（有利于放入容器）
    ThreadGuard(ThreadGuard&& other) noexcept : t(std::move(other.t)) {}
    ThreadGuard& operator=(ThreadGuard&& other) noexcept {
        if (this != &other) {
            if (t.joinable()) t.join();
            t = std::move(other.t);
        }
        return *this;
    }
};

int main() {
    if (!Logger::instance().init("app.log", LogLevel::DEBUG)) {
        std::cerr << "failed to init logger" << std::endl;
        return 1;
    }

    LOG_INFO("async logger started");

    std::vector<ThreadGuard> guards;
    guards.reserve(5);  // 预分配空间，避免重新分配时的移动问题

    // 同时启动所有线程
    for (int i = 0; i < 5; ++i) {
        // 使用 emplace_back 直接在 vector 中构造 ThreadGuard
        guards.emplace_back(std::thread([i] {
            for (int j = 0; j < 5; ++j) {
                LOG_INFO("thread " + std::to_string(i) +
                    " writes log " + std::to_string(j));
            }
            }));
    }

    LOG_WARN("all worker threads started, waiting for them to finish...");

    // 清空 vector 会触发所有 ThreadGuard 的析构，从而 join 所有线程
    guards.clear();

    LOG_WARN("all worker threads finished");
    LOG_ERROR("this is a test error message");

    Logger::instance().stop();

    std::cout << "logger demo finished, check app.log" << std::endl;
    return 0;
}