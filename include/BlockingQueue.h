#ifndef BLOCKING_QUEUE_H
#define BLOCKING_QUEUE_H

#include <condition_variable>
#include <mutex>
#include <queue>

template <typename T>
class BlockingQueue {
public:
    BlockingQueue() = default;

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    void push(const T& value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(value);
        }

        cv_.notify_one();
    }

    void push(T&& value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(value));
        }

        cv_.notify_one();
    }

    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(lock, [this] {
            return closed_ || !queue_.empty();
            });

        if (queue_.empty()) {
            return false;
        }

        value = std::move(queue_.front());
        queue_.pop();

        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }

        cv_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    bool closed_ = false;
};

#endif