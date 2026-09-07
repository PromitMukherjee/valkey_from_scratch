#include "thread_pool.hpp"

#include <algorithm>
#include <exception>
#include <iostream>

namespace koshdb {

ThreadPool::ThreadPool(std::size_t worker_count)
    : stopping(false) {
    /*
     * hardware_concurrency() may return zero when the
     * implementation cannot determine the CPU count.
     */
    if (worker_count == 0) {
        worker_count = 2;
    }

    /*
     * Background jobs should not create an excessive number
     * of threads. This is a conservative initial limit.
     */
    worker_count = std::clamp<std::size_t>(
        worker_count,
        1,
        16
    );

    workers.reserve(worker_count);

    for (std::size_t index = 0; index < worker_count; ++index) {
        workers.emplace_back(
            &ThreadPool::worker_loop,
            this
        );
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::enqueue(std::function<void()> job) {
    if (!job) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex);

        /*
         * Once shutdown begins, no new jobs are accepted.
         */
        if (stopping) {
            return;
        }

        jobs.push(std::move(job));
    }

    /*
     * Wake one sleeping worker.
     */
    condition.notify_one();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> job;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);

            /*
             * Sleep until:
             * 1. A job becomes available, or
             * 2. Shutdown begins.
             */
            condition.wait(
                lock,
                [this] {
                    return stopping || !jobs.empty();
                }
            );

            /*
             * Finish all queued jobs before exiting.
             */
            if (stopping && jobs.empty()) {
                return;
            }

            job = std::move(jobs.front());
            jobs.pop();
        }

        /*
         * The job executes outside the queue lock.
         *
         * This allows other workers to enqueue or process
         * independent background jobs.
         */
        try {
            job();
        } catch (const std::exception& error) {
            std::cerr
                << "ThreadPool job failed: "
                << error.what()
                << '\n';
        } catch (...) {
            std::cerr
                << "ThreadPool job failed with unknown error"
                << '\n';
        }
    }
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);

        if (stopping) {
            return;
        }

        stopping = true;
    }

    /*
     * Wake every worker so that they can either:
     * - finish queued jobs, or
     * - exit if the queue is empty.
     */
    condition.notify_all();

    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    workers.clear();
}

std::size_t ThreadPool::worker_count() const {
    std::lock_guard<std::mutex> lock(queue_mutex);
    return workers.size();
}

} // namespace koshdb