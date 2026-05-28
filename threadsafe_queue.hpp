#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <atomic>

// 通用线程安全队列
// 用于在线程之间传递数据
template<typename T>
class ThreadSafeQueue
{
public:
    // 拷贝方式入队
    void push(const T &value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(value);
        }
        cv_.notify_one(); // 通知等待线程有新数据
    }

    // 移动方式入队，减少拷贝开销
    void push(T &&value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(value));
        }
        cv_.notify_one();
    }

    // 非阻塞出队
    // 如果队列空，返回 false
    bool try_pop(T &out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // 阻塞等待出队
    // running = false 时退出等待，避免线程无法正常结束
    bool wait_pop(T &out, std::atomic<bool> &running)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return !queue_.empty() || !running.load(); });

        if (queue_.empty()) return false;

        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // 清空队列
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
    }

    // 唤醒所有阻塞线程
    void notify_all()
    {
        cv_.notify_all();
    }

private:
    std::queue<T> queue_;              // 底层队列
    std::mutex mutex_;                 // 互斥锁，保护队列
    std::condition_variable cv_;       // 条件变量，用于阻塞等待
};