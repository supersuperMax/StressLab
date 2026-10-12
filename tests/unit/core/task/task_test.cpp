// Task: states, progress, messages, cancellation, result and errors (task 1.6).

#include "fem/core/task/task.h"

#include "fem/core/command/value.h"
#include "fem/core/task/error.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using fem::command::Value;
using fem::task::make_task;
using fem::task::MessageSeverity;
using fem::task::Task;
using fem::task::TaskCancelled;
using fem::task::TaskControl;
using fem::task::TaskErrc;
using fem::task::TaskError;
using fem::task::TaskEvent;
using fem::task::TaskMessage;
using fem::task::TaskState;

TaskErrc error_code(const Task& task) {
    try {
        (void)task.result();
    } catch (const TaskError& error) {
        return error.code();
    }
    ADD_FAILURE() << "result() did not throw a TaskError";
    return TaskErrc::failed;
}

TEST(Task, StartsPendingWithUnknownProgress) {
    const TaskControl control = make_task("Мешинг детали");
    const Task& task = *control.task();
    EXPECT_EQ(task.title(), "Мешинг детали");
    EXPECT_EQ(task.state(), TaskState::pending);
    EXPECT_FALSE(task.is_finished());
    EXPECT_EQ(task.progress(), std::nullopt);
    EXPECT_TRUE(task.messages().empty());
    EXPECT_FALSE(task.is_cancel_requested());
    EXPECT_EQ(error_code(task), TaskErrc::not_finished);
}

TEST(Task, SucceedsWithValue) {
    TaskControl control = make_task("t");
    ASSERT_TRUE(control.start());
    EXPECT_EQ(control.task()->state(), TaskState::running);
    EXPECT_TRUE(control.succeed(Value::Dict{{"nodes", 42}}));
    const Task& task = *control.task();
    EXPECT_EQ(task.state(), TaskState::succeeded);
    EXPECT_TRUE(task.is_finished());
    EXPECT_EQ(task.result(), Value(Value::Dict{{"nodes", 42}}));
    EXPECT_EQ(task.error(), nullptr);
    EXPECT_EQ(task.error_message(), "");
}

TEST(Task, FirstFinishWins) {
    TaskControl control = make_task("t");
    control.start();
    EXPECT_TRUE(control.succeed(1));
    EXPECT_FALSE(control.fail("late"));
    EXPECT_FALSE(control.finish_cancelled());
    EXPECT_FALSE(control.succeed(2));
    EXPECT_EQ(control.task()->state(), TaskState::succeeded);
    EXPECT_EQ(control.task()->result(), Value(1));
}

TEST(Task, FailureRethrowsWhatTheWorkThrew) {
    TaskControl control = make_task("t");
    control.start();
    control.fail(std::make_exception_ptr(std::out_of_range("index 7")));
    const Task& task = *control.task();
    EXPECT_EQ(task.state(), TaskState::failed);
    EXPECT_EQ(task.error_message(), "index 7");
    EXPECT_NE(task.error(), nullptr);
    EXPECT_THROW((void)task.result(), std::out_of_range);
}

TEST(Task, FailureWithMessage) {
    TaskControl control = make_task("t");
    control.fail("no license");
    EXPECT_EQ(control.task()->state(), TaskState::failed);
    EXPECT_EQ(control.task()->error_message(), "no license");
    EXPECT_EQ(error_code(*control.task()), TaskErrc::failed);
}

TEST(Task, ProgressIsClampedAndMayBeIndeterminate) {
    TaskControl control = make_task("t");
    control.start();
    control.set_progress(0.25);
    EXPECT_EQ(control.task()->progress(), 0.25);
    control.set_progress(1.5);
    EXPECT_EQ(control.task()->progress(), 1.0);
    control.set_progress(-3);
    EXPECT_EQ(control.task()->progress(), 0.0);
    control.set_progress(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(control.task()->progress(), std::nullopt);
    control.set_progress(0.5);
    control.set_indeterminate();
    EXPECT_EQ(control.task()->progress(), std::nullopt);
}

TEST(Task, KeepsMessagesInOrder) {
    TaskControl control = make_task("t");
    control.start();
    control.message("Запись входного файла");
    control.message("Сходимость медленная", MessageSeverity::warning);
    const std::vector<TaskMessage> expected{
        {MessageSeverity::info, "Запись входного файла"},
        {MessageSeverity::warning, "Сходимость медленная"},
    };
    EXPECT_EQ(control.task()->messages(), expected);
}

TEST(Task, ChangesAfterFinishAreIgnored) {
    TaskControl control = make_task("t");
    control.start();
    control.set_progress(0.5);
    control.succeed();
    control.set_progress(0.9);
    control.message("late");
    EXPECT_EQ(control.task()->progress(), 0.5);
    EXPECT_TRUE(control.task()->messages().empty());
    EXPECT_FALSE(control.start());
}

TEST(Task, CancellingPendingTaskFinishesItAtOnce) {
    TaskControl control = make_task("t");
    const std::shared_ptr<Task> task = control.task();
    EXPECT_TRUE(task->cancel());
    EXPECT_EQ(task->state(), TaskState::cancelled);
    EXPECT_TRUE(task->is_cancel_requested());
    EXPECT_FALSE(control.start()) << "the work must not run";
    EXPECT_THROW((void)task->result(), TaskCancelled);
    EXPECT_EQ(error_code(*task), TaskErrc::cancelled);
}

TEST(Task, CancellingRunningTaskOnlyAsksTheWork) {
    TaskControl control = make_task("t");
    control.start();
    const std::shared_ptr<Task> task = control.task();
    EXPECT_TRUE(task->cancel());
    EXPECT_FALSE(task->cancel()) << "asked before";
    EXPECT_EQ(task->state(), TaskState::running);
    EXPECT_TRUE(control.is_cancel_requested());
    EXPECT_THROW(control.throw_if_cancel_requested(), TaskCancelled);
    EXPECT_TRUE(control.finish_cancelled());
    EXPECT_EQ(task->state(), TaskState::cancelled);
}

TEST(Task, WorkMayFinishAfterCancelRequest) {
    // As in .NET: a result that arrived is not thrown away.
    TaskControl control = make_task("t");
    control.start();
    control.task()->cancel();
    control.succeed(7);
    EXPECT_EQ(control.task()->state(), TaskState::succeeded);
    EXPECT_EQ(control.task()->result(), Value(7));
}

TEST(Task, CancellingFinishedTaskDoesNothing) {
    TaskControl control = make_task("t");
    control.start();
    control.succeed();
    EXPECT_FALSE(control.task()->cancel());
    EXPECT_FALSE(control.task()->is_cancel_requested());
    EXPECT_EQ(control.task()->state(), TaskState::succeeded);
}

TEST(Task, CancelCallbacksRunOnce) {
    TaskControl control = make_task("t");
    control.start();
    int calls = 0;
    control.on_cancel([&] { ++calls; });
    control.on_cancel([&] { ++calls; });
    EXPECT_EQ(calls, 0);
    control.task()->cancel();
    EXPECT_EQ(calls, 2);
    control.task()->cancel();
    EXPECT_EQ(calls, 2);
    // Registered after the request: called at once.
    control.on_cancel([&] { ++calls; });
    EXPECT_EQ(calls, 3);
}

TEST(Task, CancelCallbacksAreForgottenWhenFinished) {
    TaskControl control = make_task("t");
    control.start();
    const auto held = std::make_shared<int>(0);
    control.on_cancel([held] {});
    EXPECT_EQ(held.use_count(), 2);
    control.succeed();
    EXPECT_EQ(held.use_count(), 1);
}

TEST(Task, AbandonedWhenLastControlIsGone) {
    std::shared_ptr<Task> task;
    {
        TaskControl control = make_task("t");
        task = control.task();
        control.start();
        const TaskControl copy = control;  // NOLINT(performance-unnecessary-copy-initialization)
        (void)copy;
    }
    EXPECT_EQ(task->state(), TaskState::failed);
    EXPECT_EQ(error_code(*task), TaskErrc::abandoned);
}

TEST(Task, NotAbandonedWhileACopyLives) {
    std::optional<TaskControl> copy;
    {
        TaskControl control = make_task("t");
        copy = control;
    }
    EXPECT_EQ(copy->task()->state(), TaskState::pending);
}

TEST(Task, ListenersSeeEveryChange) {
    TaskControl control = make_task("t");
    std::vector<std::string> seen;
    const fem::Subscription subscription = control.task()->subscribe([&](const TaskEvent& event) {
        switch (event.kind) {
            case TaskEvent::Kind::state:
                seen.push_back("state " + std::string(to_string(event.state)));
                break;
            case TaskEvent::Kind::progress:
                seen.push_back("progress " +
                               (event.progress ? std::to_string(*event.progress) : "?"));
                break;
            case TaskEvent::Kind::message:
                seen.push_back("message " + event.message.text);
                break;
        }
    });
    control.start();
    control.set_progress(0.5);
    control.set_progress(0.5);  // Unchanged: no event.
    control.message("m");
    control.set_indeterminate();
    control.succeed();
    const std::vector<std::string> expected{
        "state running", "progress 0.500000", "message m", "progress ?", "state succeeded",
    };
    EXPECT_EQ(seen, expected);
}

TEST(Task, ListenerMayReadTheTask) {
    TaskControl control = make_task("t");
    const std::shared_ptr<Task> task = control.task();
    std::vector<TaskState> states;
    const fem::Subscription subscription =
        task->subscribe([&](const TaskEvent&) { states.push_back(task->state()); });
    control.start();
    control.succeed();
    EXPECT_EQ(states, (std::vector<TaskState>{TaskState::running, TaskState::succeeded}));
}

TEST(Task, WaitReturnsWhenFinishedOnAnotherThread) {
    TaskControl control = make_task("t");
    const std::shared_ptr<Task> task = control.task();
    EXPECT_FALSE(task->wait_for(10ms));
    std::thread worker([control]() mutable {
        control.start();
        std::this_thread::sleep_for(20ms);
        control.succeed(5);
    });
    task->wait();
    EXPECT_EQ(task->result(), Value(5));
    EXPECT_TRUE(task->wait_for(0ms));
    worker.join();
}

TEST(Task, ProgressFromManyThreads) {
    TaskControl control = make_task("t");
    control.start();
    std::atomic<int> events = 0;
    const fem::Subscription subscription =
        control.task()->subscribe([&](const TaskEvent&) { ++events; });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([control, t]() mutable {
            for (int i = 0; i < 200; ++i) {
                control.message("thread " + std::to_string(t));
                control.set_progress(i / 200.0);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(control.task()->messages().size(), 800U);
    EXPECT_GE(events.load(), 800);
}

TEST(TaskErrors, HaveNames) {
    EXPECT_EQ(to_string(TaskErrc::cancelled), "cancelled");
    EXPECT_EQ(to_string(TaskErrc::abandoned), "abandoned");
    EXPECT_EQ(to_string(TaskErrc::not_finished), "not_finished");
    EXPECT_EQ(to_string(TaskErrc::failed), "failed");
    EXPECT_EQ(to_string(TaskState::cancelled), "cancelled");
    EXPECT_EQ(to_string(MessageSeverity::warning), "warning");
}

}  // namespace
