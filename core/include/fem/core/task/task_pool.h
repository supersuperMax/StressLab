#pragma once

#include "fem/core/command/value.h"
#include "fem/core/task/task.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fem::task {

/// Runs functions as tasks on a fixed set of threads, in the order they were submitted.
///
/// What the function returns is the result of the task; TaskCancelled thrown from it cancels the
/// task, anything else thrown fails it. A function cancelled while it waits in the queue does not
/// run.
class TaskPool {
public:
    using Work = std::function<command::Value(TaskControl&)>;

    /// `threads` = 0 takes one thread per hardware thread.
    explicit TaskPool(std::size_t threads = 0);
    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;
    TaskPool(TaskPool&&) = delete;
    TaskPool& operator=(TaskPool&&) = delete;
    /// Cancels every task of the pool and waits for the running ones to stop.
    ~TaskPool();

    std::shared_ptr<Task> submit(std::string title, Work work);

    [[nodiscard]] std::size_t thread_count() const noexcept { return threads_.size(); }

private:
    struct Item {
        TaskControl control;
        Work work;
    };

    void run();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Item> queue_;
    std::vector<std::shared_ptr<Task>> running_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace fem::task
