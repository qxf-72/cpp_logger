#ifndef BLOCKING_QUEUE_H
#define BLOCKING_QUEUE_H

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

template <typename T>
class BlockingQueue {
public:
    BlockingQueue() = default;

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

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

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    bool closed_ = false;
};

#endif