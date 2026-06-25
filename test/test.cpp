#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "Logger.h"

class ThreadGuard {
  std::thread t;  // 直接持有线程对象，确保 ThreadGuard 能负责它的完整生命周期。

 public:
  // 接收一个临时 std::thread，并通过移动语义转移所有权。
  explicit ThreadGuard(std::thread t_) : t(std::move(t_)) {}

  ~ThreadGuard() {
    // RAII 兜底：对象析构时自动 join，避免线程未回收导致 std::terminate。
    if (t.joinable()) {
      t.join();
    }
  }

  ThreadGuard(const ThreadGuard&) = delete;
  ThreadGuard& operator=(const ThreadGuard&) = delete;

  // 支持移动构造，方便放入 vector；拷贝被禁止，因为 std::thread 本身不可拷贝。
  ThreadGuard(ThreadGuard&& other) noexcept : t(std::move(other.t)) {}
  ThreadGuard& operator=(ThreadGuard&& other) noexcept {
    if (this != &other) {
      // 覆盖已有线程前先 join，防止原线程句柄被丢弃。
      if (t.joinable())
        t.join();
      t = std::move(other.t);
    }
    return *this;
  }
};

int main() {
  if (!Logger::instance().init("app", LogLevel::DEBUG)) {
    std::cerr << "failed to init logger" << std::endl;
    return 1;
  }

  LOG_INFO("async logger started");

  std::vector<ThreadGuard> guards;
  guards.reserve(5);

  // 同时启动多个业务线程，模拟高并发场景下频繁写日志。
  // 这里每个线程写入 10000 条日志，用来观察异步队列和文件滚动逻辑是否稳定。
  for (int i = 0; i < 5; ++i) {
    guards.emplace_back(std::thread([i] {
      for (int j = 0; j < 10000; ++j) {
        LOG_INFO("thread " + std::to_string(i) + " writes log " + std::to_string(j));
      }
    }));
  }

  LOG_WARN("all worker threads started, waiting for them to finish...");

  // 清空 vector 会触发所有 ThreadGuard 析构，从而等待所有业务线程结束。
  // 业务线程结束后再 stop logger，可以保证队列里的日志都有机会被后台线程写完。
  guards.clear();

  LOG_WARN("all worker threads finished");
  LOG_ERROR("this is a test error message");
  LOG_FATAL("this is a test fatal message");

  Logger::instance().stop();

  std::cout << "logger demo finished, check app.log" << std::endl;
  return 0;
}
