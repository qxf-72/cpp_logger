#ifndef BLOCKING_QUEUE_H
#define BLOCKING_QUEUE_H

#include <condition_variable>  // std::condition_variable，用于线程等待和唤醒
#include <cstddef>             // std::size_t
#include <mutex>               // std::mutex, std::lock_guard, std::unique_lock
#include <queue>               // std::queue
#include <utility>             // std::move

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
            // 加锁，保护 queue_ 和 closed_
            // 因为它们可能被多个线程同时访问
            std::lock_guard<std::mutex> lock(mutex_);

            // 如果队列已经关闭，就不再接受新数据
            if (closed_) {
                return false;
            }

            // 将数据放入队列
            queue_.push(value);
        }

        // 解锁后通知一个等待中的消费者线程
        // 如果后台线程正在 pop() 中等待，这里会把它唤醒
        cv_.notify_one();

        return true;
    }

    // push 右值版本
    // 适用于 queue.push(std::move(msg)) 或 queue.push("hello") 这种情况
    // 可以减少不必要的拷贝
    bool push(T&& value) {
        {
            // 加锁，保护共享队列
            std::lock_guard<std::mutex> lock(mutex_);

            // 队列关闭后拒绝写入
            if (closed_) {
                return false;
            }

            // 使用移动语义把 value 放入队列
            // 对于 std::string 这种对象，可以减少拷贝成本
            queue_.push(std::move(value));
        }

        // 通知一个消费者：队列中有新数据了
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
        // condition_variable::wait 需要 unique_lock
        // 因为 wait 内部需要临时释放锁，并在被唤醒后重新加锁
        std::unique_lock<std::mutex> lock(mutex_);

        // 如果队列为空，并且没有关闭，消费者线程就在这里阻塞等待
        //
        // wait 的第二个参数是谓词 predicate
        // 只有当 closed_ == true 或 queue_ 非空时，才会继续执行
        //
        // 使用谓词写法可以防止“虚假唤醒”
        cv_.wait(lock, [this] {
            return closed_ || !queue_.empty();
            });

        // 运行到这里，说明满足以下条件之一：
        // 1. 队列非空
        // 2. 队列被关闭
        //
        // 如果队列为空，说明是因为 close() 被唤醒的
        // 此时已经没有数据可取，返回 false，通知消费者线程可以退出
        if (queue_.empty()) {
            return false;
        }

        // 取出队头元素
        // 使用 move 可以减少拷贝
        value = std::move(queue_.front());

        // 删除队头元素
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
            // 加锁修改 closed_
            std::lock_guard<std::mutex> lock(mutex_);

            // 标记队列已经关闭
            closed_ = true;
        }

        // 唤醒所有等待中的消费者线程
        // 如果后台线程正阻塞在 pop()，它会被唤醒
        cv_.notify_all();
    }

    // 判断队列是否为空
    bool empty() const {
        // 即使是 const 函数，也需要加锁读取 queue_
        // 因为其他线程可能正在 push 或 pop
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    // 返回当前队列大小
    std::size_t size() const {
        // 加锁读取 queue_.size()
        // 避免和其他线程的 push/pop 冲突
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // 判断队列是否已经关闭
    bool closed() const {
        // closed_ 也是共享变量，读取时同样加锁
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

    // 条件变量
    // 用于在队列为空时阻塞消费者线程
    // push() 或 close() 时会通知等待线程
    std::condition_variable cv_;

    // 真正存储数据的队列
    std::queue<T> queue_;

    // 标记队列是否已经关闭
    //
    // false：队列正常工作，可以 push，可以 pop
    // true ：队列关闭，不再接受 push，pop 取完剩余数据后返回 false
    bool closed_ = false;
};

#endif