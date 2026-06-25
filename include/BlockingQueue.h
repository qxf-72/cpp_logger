#ifndef BLOCKING_QUEUE_H
#define BLOCKING_QUEUE_H

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <type_traits>
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

  // 支持左值和右值入队：对 string 这类对象可以避免一次不必要的拷贝。
  // 返回 false 表示队列已经关闭，调用方应停止继续投递新任务。
  template <typename U,
            typename = typename std::enable_if<std::is_constructible<T, U&&>::value>::type>
  bool push(U&& value) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (close_) {
        return false;
      }
      que_.emplace(std::forward<U>(value));
    }

    cv_.notify_one();
    return true;
  }

  // 阻塞式出队：
  // 1. 队列为空且未关闭时，消费者线程会在条件变量上等待。
  // 2. 队列关闭后仍会先取完剩余元素，只有队列真的为空时才返回 false。
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

  // 关闭队列并唤醒所有等待线程。
  // 关闭后 push 会失败，但 pop 仍允许消费队列中已经存在的数据。
  void close() {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      close_ = true;
    }
    cv_.notify_all();
  }

  // 重新打开队列，供 Logger 在 stop() 之后再次 init() 使用。
  // reset 会清空残留数据，避免上一次运行未消费完的日志污染下一次运行。
  void reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    std::queue<T> empty;
    que_.swap(empty);
    close_ = false;
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
