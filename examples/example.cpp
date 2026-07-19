#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "Logger.h"

int main() {
  // 使用配置结构统一设置文件滚动、队列容量和队列满时的处理方式。
  LoggerConfig config;
  config.basePath = "logs/app";
  config.minLevel = LogLevel::DEBUG;
  config.queueCapacity = 8192;
  config.overflowPolicy = OverflowPolicy::Block;
  if (!Logger::instance().init(config)) {
    std::cerr << "failed to initialize logger\n";
    return 1;
  }

  LOG_INFO("async logger started");

  constexpr int kThreadCount = 5;
  constexpr int kMessagesPerThread = 10000;
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);

  // 线程创建部分失败时也能回收已创建的线程，避免 std::thread 析构触发 terminate。
  const auto joinWorkers = [&workers] {
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  };

  try {
    // 模拟多个业务线程同时提交日志。
    for (int threadIndex = 0; threadIndex < kThreadCount; ++threadIndex) {
      workers.emplace_back([threadIndex] {
        for (int messageIndex = 0; messageIndex < kMessagesPerThread; ++messageIndex) {
          LOG_INFO("thread " + std::to_string(threadIndex) + " writes log " +
                   std::to_string(messageIndex));
        }
      });
    }
  } catch (const std::exception& error) {
    joinWorkers();
    Logger::instance().stop();
    std::cerr << "failed to create worker thread: " << error.what() << '\n';
    return 1;
  }

  LOG_WARN("all worker threads started, waiting for them to finish");
  joinWorkers();

  LOG_WARN("all worker threads finished");
  LOG_ERROR("this is a test error message");
  LOG_FATAL("this is a test fatal message");
  Logger::instance().stop();

  std::cout << "logger demo finished, dropped=" << Logger::instance().droppedCount()
            << ", check logs/app_*.log\n";
  return 0;
}
