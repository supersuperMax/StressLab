#include "fem/core/task/task_pool.h"

#include "fem/core/task/error.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace fem::task {

TaskPool::TaskPool(std::size_t threads) {
    if (threads == 0) {
        threads = std::max(1U, std::thread::hardware_concurrency());
    }
    threads_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        threads_.emplace_back([this] { run(); });
    }
}

TaskPool::~TaskPool() {
    std::deque<Item> queued;
    std::vector<std::shared_ptr<Task>> running;
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        queued = std::exchange(queue_, {});
        running = running_;
    }
    wake_.notify_all();
    for (const Item& item : queued) {
        item.control.task()->cancel();
    }
    for (const std::shared_ptr<Task>& task : running) {
        task->cancel();
    }
    for (std::thread& thread : threads_) {
        thread.join();
    }
}

std::shared_ptr<Task> TaskPool::submit(std::string title, Work work) {
    TaskControl control = make_task(std::move(title));
    std::shared_ptr<Task> task = control.task();
    {
        const std::lock_guard lock(mutex_);
        queue_.push_back(Item{std::move(control), std::move(work)});
    }
    wake_.notify_one();
    return task;
}

void TaskPool::run() {
    for (;;) {
        std::optional<Item> item;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            item.emplace(std::move(queue_.front()));
            queue_.pop_front();
            running_.push_back(item->control.task());
        }
        TaskControl& control = item->control;
        if (control.start()) {
            try {
                control.succeed(item->work(control));
            } catch (const TaskCancelled&) {
                control.finish_cancelled();
            } catch (...) {
                control.fail(std::current_exception());
            }
        }
        {
            const std::lock_guard lock(mutex_);
            std::erase(running_, control.task());
        }
    }
}

}  // namespace fem::task
