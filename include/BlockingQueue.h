#ifndef BLOCKING_QUEUE_H
#define BLOCKING_QUEUE_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <type_traits>
#include <utility>
#include <vector>

// 队列达到容量上限时的处理方式。
enum class OverflowPolicy { Block, DropNewest, DropOldest };

// 入队结果区分关闭和溢出丢弃，便于上层准确统计丢弃日志。
enum class QueuePushResult { Enqueued, DroppedNewest, DroppedOldest, Closed };

template <typename T>
class BlockingQueue {
 public:
  BlockingQueue() = default;
  BlockingQueue(const BlockingQueue&) = delete;
  BlockingQueue& operator=(const BlockingQueue&) = delete;

  // 使用当前代次入队，供独立使用 BlockingQueue 的场景调用。
  template <typename U, std::enable_if_t<std::is_constructible_v<T, U&&>, int> = 0>
  [[nodiscard]] QueuePushResult push(U&& value) {
    std::unique_lock lock(mutex_);
    return pushLocked(lock, generation_, std::forward<U>(value));
  }

  // expectedGeneration 用于 Logger 的生命周期同步：重启后，旧运行周期中尚未完成的
  // 生产者不会把消息写入新队列。
  template <typename U, std::enable_if_t<std::is_constructible_v<T, U&&>, int> = 0>
  [[nodiscard]] QueuePushResult push(std::size_t expectedGeneration, U&& value) {
    std::unique_lock lock(mutex_);
    return pushLocked(lock, expectedGeneration, std::forward<U>(value));
  }

  // 队列关闭且元素已取尽时返回 std::nullopt；关闭前已入队的元素仍会被正常消费。
  [[nodiscard]] std::optional<T> pop() {
    std::unique_lock lock(mutex_);
    notEmpty_.wait(lock, [this] { return closed_ || !queue_.empty(); });

    if (queue_.empty()) {
      return std::nullopt;
    }

    T value = std::move(queue_.front());
    queue_.pop();
    lock.unlock();
    // 为 Block 策略中等待空位的生产者腾出一个位置。
    notFull_.notify_one();
    return value;
  }

  // 一次取出至多 maxCount 个元素，减少消费者频繁加锁和上下文切换的开销。
  // 队列关闭且已排空时返回 false。
  [[nodiscard]] bool popBatch(std::vector<T>& values, std::size_t maxCount) {
    return popBatchImpl(values, maxCount, [this](std::unique_lock<std::mutex>& lock) {
      notEmpty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
      return true;
    });
  }

  // 最多等待 timeout 后返回，供需要定时执行维护工作的消费者使用。
  template <typename Rep, typename Period>
  [[nodiscard]] bool popBatchFor(std::vector<T>& values, std::size_t maxCount,
                                 const std::chrono::duration<Rep, Period>& timeout) {
    return popBatchImpl(values, maxCount, [this, &timeout](std::unique_lock<std::mutex>& lock) {
      return notEmpty_.wait_for(lock, timeout, [this] { return closed_ || !queue_.empty(); });
    });
  }

  // 唤醒消费者与因队列已满而阻塞的生产者。
  void close() {
    {
      std::scoped_lock lock(mutex_);
      closed_ = true;
    }
    notEmpty_.notify_all();
    notFull_.notify_all();
  }

  // 保留旧接口的无容量上限语义，便于 BlockingQueue 独立使用时迁移。
  void reset() {
    reset(std::numeric_limits<std::size_t>::max(), OverflowPolicy::Block);
  }

  // Logger 仅会在旧 worker 已退出后调用。每次重置都会开始一个新代次并清空统计数据。
  void reset(std::size_t capacity, OverflowPolicy overflowPolicy) {
    std::scoped_lock lock(mutex_);
    std::queue<T>{}.swap(queue_);
    // Logger 会拒绝容量为 0 的配置；此处仍做防御性保护，避免独立使用时永久阻塞。
    capacity_ = capacity == 0 ? 1 : capacity;
    overflowPolicy_ = overflowPolicy;
    highWaterMark_ = 0;
    droppedCount_ = 0;
    closed_ = false;
    ++generation_;
    notEmpty_.notify_all();
    notFull_.notify_all();
  }

  [[nodiscard]] bool empty() const {
    std::scoped_lock lock(mutex_);
    return queue_.empty();
  }

  [[nodiscard]] std::size_t size() const {
    std::scoped_lock lock(mutex_);
    return queue_.size();
  }

  [[nodiscard]] std::size_t peakSize() const {
    std::scoped_lock lock(mutex_);
    return highWaterMark_;
  }

  [[nodiscard]] std::uint64_t droppedCount() const {
    std::scoped_lock lock(mutex_);
    return droppedCount_;
  }

  [[nodiscard]] std::size_t capacity() const {
    std::scoped_lock lock(mutex_);
    return capacity_;
  }

  [[nodiscard]] OverflowPolicy overflowPolicy() const {
    std::scoped_lock lock(mutex_);
    return overflowPolicy_;
  }

  [[nodiscard]] std::size_t generation() const {
    std::scoped_lock lock(mutex_);
    return generation_;
  }

  [[nodiscard]] bool closed() const {
    std::scoped_lock lock(mutex_);
    return closed_;
  }

 private:
  template <typename WaitFunction>
  [[nodiscard]] bool popBatchImpl(std::vector<T>& values, std::size_t maxCount,
                                  WaitFunction&& waitForData) {
    values.clear();
    if (maxCount == 0) {
      return false;
    }

    std::unique_lock lock(mutex_);
    if (!waitForData(lock)) {
      return false;
    }

    if (queue_.empty()) {
      return false;
    }

    if (values.capacity() < maxCount) {
      values.reserve(maxCount);
    }
    for (std::size_t index = 0; index < maxCount && !queue_.empty(); ++index) {
      values.emplace_back(std::move(queue_.front()));
      queue_.pop();
    }
    lock.unlock();
    // 可能一次释放了多个空位，唤醒所有等待的 Block 生产者。
    notFull_.notify_all();
    return true;
  }
  template <typename U>
  QueuePushResult pushLocked(std::unique_lock<std::mutex>& lock, std::size_t expectedGeneration,
                             U&& value) {
    if (closed_ || expectedGeneration != generation_) {
      return QueuePushResult::Closed;
    }

    if (overflowPolicy_ == OverflowPolicy::Block) {
      notFull_.wait(lock, [this, expectedGeneration] {
        return closed_ || expectedGeneration != generation_ || queue_.size() < capacity_;
      });
      if (closed_ || expectedGeneration != generation_) {
        return QueuePushResult::Closed;
      }
    } else if (queue_.size() >= capacity_) {
      if (overflowPolicy_ == OverflowPolicy::DropNewest) {
        ++droppedCount_;
        return QueuePushResult::DroppedNewest;
      }

      // DropOldest：移除最早的消息，再让新消息进入队列。
      queue_.pop();
      ++droppedCount_;
      emplaceLocked(std::forward<U>(value));
      lock.unlock();
      notEmpty_.notify_one();
      return QueuePushResult::DroppedOldest;
    }

    emplaceLocked(std::forward<U>(value));
    lock.unlock();
    notEmpty_.notify_one();
    return QueuePushResult::Enqueued;
  }

  template <typename U>
  void emplaceLocked(U&& value) {
    queue_.emplace(std::forward<U>(value));
    if (queue_.size() > highWaterMark_) {
      highWaterMark_ = queue_.size();
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable notEmpty_;
  std::condition_variable notFull_;
  std::queue<T> queue_;
  std::size_t capacity_{std::numeric_limits<std::size_t>::max()};
  std::size_t highWaterMark_{0};
  std::size_t generation_{0};
  std::uint64_t droppedCount_{0};
  OverflowPolicy overflowPolicy_{OverflowPolicy::Block};
  bool closed_{false};
};

#endif
