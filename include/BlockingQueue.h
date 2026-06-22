#ifndef BLOCKING_QUEUE_H
#define BLOCKING_QUEUE_H

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

template <typename T>
class BlockingQueue {
 private:
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::queue<T> que_;
  bool close_{false};

 public:
  BlockingQueue() = default;
  BlockingQueue& operator=(const BlockingQueue&) = delete;
  BlockingQueue(const BlockingQueue&) = delete;

  bool push(const T& val) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (close_) {
        return false;
      }
      que_.push(val);
    }
    cv_.notify_one();
    return true;
  }

  bool push(T&& val) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (close_) {
        return false;
      }
      que_.push(val);
    }
    cv_.notify_one();
    return true;
  }

  bool pop(T& val) {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this]() -> bool { return close_ || !que_.empty(); });

    if (que_.empty()) {
      return false;
    }

    val = std::move(que_.front());
    que_.pop();

    return true;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      close_ = true;
    }
    cv_.notify_all();  // 唤醒后台等待的线程
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return que_.empty();
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return que_.size();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return close_;
  }
};

#endif
