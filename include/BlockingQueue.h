#ifndef BLOCKING_QUEUE_H
#define BLOCKING_QUEUE_H

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

// BlockingQueue 是一个线程安全的阻塞队列
// T 表示队列中存储的数据类型
//
// 在异步日志系统中，T 通常是 std::string：
// BlockingQueue<std::string> queue_;
//
// 生产者线程：业务线程，负责 push 日志
// 消费者线程：后台日志线程，负责 pop 日志并写文件
template <typename T>
class BlockingQueue {
 public:
  BlockingQueue() = default;

  // 禁止拷贝
  // 因为 BlockingQueue 内部有 mutex 和 condition_variable
  // 这些同步对象不适合被复制
  BlockingQueue(const BlockingQueue&) = delete;
  BlockingQueue& operator=(const BlockingQueue&) = delete;

  // push 左值版本
  // 适用于 queue.push(msg) 这种情况
  // 会把 value 拷贝一份放入队列
  bool push(const T& value) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return false;
      }
      queue_.push(value);
    }

    cv_.notify_one();
    return true;
  }

  // push 右值版本
  // 适用于 queue.push(std::move(msg)) 或 queue.push("hello") 这种情况
  // 可以减少不必要的拷贝
  bool push(T&& value) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return false;
      }
      queue_.push(std::move(value));
    }

    cv_.notify_one();
    return true;
  }

  // pop 用于消费者线程取数据
  //
  // 返回 true：
  //     成功取出一个元素，结果放入 value
  //
  // 返回 false：
  //     队列已经关闭，并且队列中没有剩余元素
  //
  // 如果队列为空且没有关闭：
  //     当前线程会阻塞等待
  bool pop(T& value) {
    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });

    if (queue_.empty()) {
      return false;
    }

    value = std::move(queue_.front());
    queue_.pop();

    return true;
  }

  // 关闭队列
  //
  // close() 的含义是：
  //     不再接受新的 push()
  //     唤醒所有正在等待的 pop()
  //
  // 在异步日志中，Logger::stop() 会调用 queue_.close()
  // 让后台日志线程有机会退出
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }

    cv_.notify_all();
  }

  // 判断队列是否为空
  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  // 返回当前队列大小
  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  // 判断队列是否已经关闭
  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

 private:
  // mutex_ 用于保护下面这些共享数据：
  // queue_
  // closed_
  //
  // mutable 的作用：
  // 允许在 const 成员函数 empty() / size() / closed() 中给 mutex_ 加锁
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<T> queue_;
  bool closed_ = false;
};

#endif
