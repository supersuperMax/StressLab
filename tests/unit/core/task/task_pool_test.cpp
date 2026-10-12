// TaskPool: functions run as tasks on a fixed set of threads (task 1.6).

#include "fem/core/task/task_pool.h"

#include "fem/core/command/value.h"
#include "fem/core/task/error.h"
#include "fem/core/task/task.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using fem::command::Value;
using fem::task::Task;
using fem::task::TaskCancelled;
using fem::task::TaskControl;
using fem::task::TaskPool;
using fem::task::TaskState;

constexpr auto timeout = 10s;

/// A gate the work waits at until the test opens it.
class Gate {
public:
    void open() {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    void wait() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

TEST(TaskPool, RunsWorkAndKeepsResult) {
    TaskPool pool(2);
    EXPECT_EQ(pool.thread_count(), 2U);
    const std::shared_ptr<Task> task = pool.submit("sum", [](TaskControl& control) -> Value {
        control.set_progress(0.5);
        control.message("half");
        return 1 + 2;
    });
    EXPECT_EQ(task->title(), "sum");
    ASSERT_TRUE(task->wait_for(timeout));
    EXPECT_EQ(task->state(), TaskState::succeeded);
    EXPECT_EQ(task->result(), Value(3));
    EXPECT_EQ(task->progress(), 0.5);
    EXPECT_EQ(task->messages().size(), 1U);
}

TEST(TaskPool, DefaultUsesHardwareThreads) {
    const TaskPool pool;
    EXPECT_GE(pool.thread_count(), 1U);
}

TEST(TaskPool, ExceptionFailsTheTask) {
    TaskPool pool(1);
    const std::shared_ptr<Task> task = pool.submit(
        "bad", [](TaskControl&) -> Value { throw std::runtime_error("singular matrix"); });
    ASSERT_TRUE(task->wait_for(timeout));
    EXPECT_EQ(task->state(), TaskState::failed);
    EXPECT_EQ(task->error_message(), "singular matrix");
    EXPECT_THROW((void)task->result(), std::runtime_error);
}

TEST(TaskPool, RunsInSubmissionOrderOnOneThread) {
    TaskPool pool(1);
    std::vector<int> order;
    std::mutex mutex;
    std::vector<std::shared_ptr<Task>> tasks;
    for (int i = 0; i < 20; ++i) {
        tasks.push_back(pool.submit("t", [&, i](TaskControl&) -> Value {
            const std::lock_guard lock(mutex);
            order.push_back(i);
            return {};
        }));
    }
    for (const auto& task : tasks) {
        ASSERT_TRUE(task->wait_for(timeout));
    }
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(order[i], i);
    }
}

TEST(TaskPool, RunsTasksInParallel) {
    TaskPool pool(3);
    std::atomic<int> inside = 0;
    Gate gate;
    std::vector<std::shared_ptr<Task>> tasks;
    for (int i = 0; i < 3; ++i) {
        tasks.push_back(pool.submit("t", [&](TaskControl&) -> Value {
            ++inside;
            gate.wait();
            return {};
        }));
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (inside.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(inside.load(), 3) << "all three must run at once";
    gate.open();
    for (const auto& task : tasks) {
        ASSERT_TRUE(task->wait_for(timeout));
    }
}

TEST(TaskPool, CancelledWhileQueuedDoesNotRun) {
    TaskPool pool(1);
    Gate gate;
    const std::shared_ptr<Task> blocker = pool.submit("blocker", [&](TaskControl&) -> Value {
        gate.wait();
        return {};
    });
    std::atomic<bool> ran = false;
    const std::shared_ptr<Task> queued = pool.submit("queued", [&](TaskControl&) -> Value {
        ran = true;
        return {};
    });
    EXPECT_TRUE(queued->cancel());
    EXPECT_EQ(queued->state(), TaskState::cancelled);
    gate.open();
    ASSERT_TRUE(blocker->wait_for(timeout));
    // A third task proves the queue moved past the cancelled one.
    const std::shared_ptr<Task> after =
        pool.submit("after", [](TaskControl&) -> Value { return 1; });
    ASSERT_TRUE(after->wait_for(timeout));
    EXPECT_FALSE(ran.load());
}

TEST(TaskPool, WorkStopsWhenCancelled) {
    TaskPool pool(1);
    std::atomic<bool> started = false;
    const std::shared_ptr<Task> task = pool.submit("loop", [&](TaskControl& control) -> Value {
        started = true;
        for (;;) {
            control.throw_if_cancel_requested();
            std::this_thread::sleep_for(1ms);
        }
    });
    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }
    task->cancel();
    ASSERT_TRUE(task->wait_for(timeout));
    EXPECT_EQ(task->state(), TaskState::cancelled);
    EXPECT_THROW((void)task->result(), TaskCancelled);
}

TEST(TaskPool, DestructorCancelsAndWaits) {
    std::shared_ptr<Task> running;
    std::shared_ptr<Task> queued;
    std::atomic<bool> started = false;
    {
        TaskPool pool(1);
        running = pool.submit("loop", [&](TaskControl& control) -> Value {
            started = true;
            while (!control.is_cancel_requested()) {
                std::this_thread::sleep_for(1ms);
            }
            control.throw_if_cancel_requested();
            return {};
        });
        queued = pool.submit("queued", [](TaskControl&) -> Value { return {}; });
        while (!started.load()) {
            std::this_thread::sleep_for(1ms);
        }
    }
    EXPECT_EQ(running->state(), TaskState::cancelled);
    EXPECT_EQ(queued->state(), TaskState::cancelled);
}

}  // namespace
