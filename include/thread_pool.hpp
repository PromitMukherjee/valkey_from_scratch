#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace koshdb {

class ThreadPool {
public:
    explicit ThreadPool(
        std::size_t worker_count = std::thread::hardware_concurrency()
    );

    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /*
     * Add a background job to the queue.
     *
     * Example:
     * thread_pool.enqueue([] {
     *     // persistence or replication work
     * });
     */
    void enqueue(std::function<void()> job);

    /*
     * Gracefully stop accepting new jobs and wait for
     * already queued jobs to finish.
     */
    void shutdown();

    std::size_t worker_count() const;

private:
    void worker_loop();

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> jobs;

    mutable std::mutex queue_mutex;
    std::condition_variable condition;

    bool stopping;
};

} // namespace koshdb