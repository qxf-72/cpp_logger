#include "Logger.h"

#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main() {
    if (!Logger::instance().init("app.log", LogLevel::DEBUG)) {
        std::cerr << "failed to init logger" << std::endl;
        return 1;
    }

    LOG_INFO("async logger started");

    std::vector<std::thread> threads;

    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([i] {
            for (int j = 0; j < 10; ++j) {
                LOG_INFO("thread " + std::to_string(i) +
                    " writes log " + std::to_string(j));
            }
            });
    }

    for (auto& t : threads) {
        t.join();
    }

    LOG_WARN("all worker threads finished");
    LOG_ERROR("this is a test error message");

    Logger::instance().stop();

    std::cout << "logger demo finished, check app.log" << std::endl;

    return 0;
}