#include "fem/core/task/task.h"

#include "fem/core/task/error.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace fem::task {

std::string_view to_string(TaskState state) noexcept {
    switch (state) {
        case TaskState::pending:
            return "pending";
        case TaskState::running:
            return "running";
        case TaskState::succeeded:
            return "succeeded";
        case TaskState::failed:
            return "failed";
        case TaskState::cancelled:
            return "cancelled";
    }
    return "unknown";
}

bool is_finished(TaskState state) noexcept {
    return state == TaskState::succeeded || state == TaskState::failed ||
           state == TaskState::cancelled;
}

std::string_view to_string(MessageSeverity severity) noexcept {
    switch (severity) {
        case MessageSeverity::info:
            return "info";
        case MessageSeverity::warning:
            return "warning";
        case MessageSeverity::error:
            return "error";
    }
    return "unknown";
}

namespace {

TaskEvent event(TaskEvent::Kind kind, TaskState state, std::optional<double> progress,
                TaskMessage message = {}) {
    TaskEvent result;
    result.kind = kind;
    result.state = state;
    result.progress = progress;
    result.message = std::move(message);
    return result;
}

}  // namespace

Task::Task(std::string title) : title_(std::move(title)) {}

TaskState Task::state() const {
    const std::lock_guard lock(mutex_);
    return state_;
}

bool Task::is_finished() const {
    const std::lock_guard lock(mutex_);
    return task::is_finished(state_);
}

std::optional<double> Task::progress() const {
    const std::lock_guard lock(mutex_);
    return progress_;
}

std::vector<TaskMessage> Task::messages() const {
    const std::lock_guard lock(mutex_);
    return messages_;
}

bool Task::cancel() {
    std::vector<std::function<void()>> callbacks;
    bool cancelled_now = false;
    {
        const std::lock_guard lock(mutex_);
        if (task::is_finished(state_) || cancel_requested_) {
            return false;
        }
        cancel_requested_ = true;
        callbacks = std::exchange(cancel_callbacks_, {});
        if (state_ == TaskState::pending) {
            state_ = TaskState::cancelled;
            cancelled_now = true;
        }
    }
    for (const std::function<void()>& callback : callbacks) {
        callback();
    }
    if (cancelled_now) {
        finished_.notify_all();
        changed_.emit(event(TaskEvent::Kind::state, TaskState::cancelled, progress()));
    }
    return true;
}

bool Task::is_cancel_requested() const {
    const std::lock_guard lock(mutex_);
    return cancel_requested_;
}

void Task::wait() const {
    std::unique_lock lock(mutex_);
    finished_.wait(lock, [this] { return task::is_finished(state_); });
}

command::Value Task::result() const {
    const std::lock_guard lock(mutex_);
    switch (state_) {
        case TaskState::succeeded:
            return value_;
        case TaskState::failed:
            std::rethrow_exception(error_);
        case TaskState::cancelled:
            throw TaskCancelled();
        case TaskState::pending:
        case TaskState::running:
            break;
    }
    throw TaskError(TaskErrc::not_finished, "task '" + title_ + "' has not finished");
}

std::exception_ptr Task::error() const {
    const std::lock_guard lock(mutex_);
    return error_;
}

std::string Task::error_message() const {
    const std::exception_ptr error = this->error();
    if (!error) {
        return {};
    }
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& exception) {
        return exception.what();
    } catch (...) {
        return "unknown error";
    }
}

Subscription Task::subscribe(Listener listener) {
    return changed_.subscribe(std::move(listener));
}

bool Task::start() {
    std::optional<double> progress;
    {
        const std::lock_guard lock(mutex_);
        if (state_ != TaskState::pending) {
            return false;
        }
        state_ = TaskState::running;
        progress = progress_;
    }
    changed_.emit(event(TaskEvent::Kind::state, TaskState::running, progress));
    return true;
}

void Task::set_progress(std::optional<double> progress) {
    TaskState state{};
    {
        const std::lock_guard lock(mutex_);
        if (task::is_finished(state_) || progress_ == progress) {
            return;
        }
        progress_ = progress;
        state = state_;
    }
    changed_.emit(event(TaskEvent::Kind::progress, state, progress));
}

void Task::add_message(TaskMessage message) {
    TaskState state{};
    std::optional<double> progress;
    {
        const std::lock_guard lock(mutex_);
        if (task::is_finished(state_)) {
            return;
        }
        messages_.push_back(message);
        state = state_;
        progress = progress_;
    }
    changed_.emit(event(TaskEvent::Kind::message, state, progress, std::move(message)));
}

void Task::on_cancel(std::function<void()> callback) {
    {
        const std::lock_guard lock(mutex_);
        if (task::is_finished(state_)) {
            return;
        }
        if (!cancel_requested_) {
            cancel_callbacks_.push_back(std::move(callback));
            return;
        }
    }
    callback();
}

bool Task::finish(TaskState state, command::Value value, std::exception_ptr error) {
    std::optional<double> progress;
    std::vector<std::function<void()>> forgotten;
    {
        const std::lock_guard lock(mutex_);
        if (task::is_finished(state_)) {
            return false;
        }
        state_ = state;
        value_ = std::move(value);
        error_ = std::move(error);
        progress = progress_;
        forgotten = std::exchange(cancel_callbacks_, {});
    }
    // Callbacks are destroyed outside the lock: what they hold may lead back to this task.
    forgotten.clear();
    finished_.notify_all();
    changed_.emit(event(TaskEvent::Kind::state, state, progress));
    return true;
}

// The shared part of all copies of a TaskControl: when the last one goes, an unfinished task is
// abandoned.
struct TaskControl::Owner {
    explicit Owner(std::shared_ptr<Task> task) : task(std::move(task)) {}
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    Owner(Owner&&) = delete;
    Owner& operator=(Owner&&) = delete;
    ~Owner() {
        try {
            task->finish(TaskState::failed, {},
                         std::make_exception_ptr(TaskError(
                             TaskErrc::abandoned,
                             "task '" + task->title() + "' was abandoned before it finished")));
        } catch (...) {
            // A listener threw; nothing can be reported from a destructor.
        }
    }

    const std::shared_ptr<Task> task;
};

TaskControl::TaskControl(std::shared_ptr<Owner> owner) : owner_(std::move(owner)) {}

const std::shared_ptr<Task>& TaskControl::task() const noexcept {
    return owner_->task;
}

bool TaskControl::start() {
    return owner_->task->start();
}

void TaskControl::set_progress(double fraction) {
    if (std::isnan(fraction)) {
        owner_->task->set_progress(std::nullopt);
        return;
    }
    owner_->task->set_progress(std::clamp(fraction, 0.0, 1.0));
}

void TaskControl::set_indeterminate() {
    owner_->task->set_progress(std::nullopt);
}

void TaskControl::message(std::string text, MessageSeverity severity) {
    owner_->task->add_message(TaskMessage{severity, std::move(text)});
}

bool TaskControl::is_cancel_requested() const {
    return owner_->task->is_cancel_requested();
}

void TaskControl::throw_if_cancel_requested() const {
    if (is_cancel_requested()) {
        throw TaskCancelled();
    }
}

void TaskControl::on_cancel(std::function<void()> callback) {
    owner_->task->on_cancel(std::move(callback));
}

bool TaskControl::succeed(command::Value value) {
    return owner_->task->finish(TaskState::succeeded, std::move(value), nullptr);
}

bool TaskControl::fail(std::exception_ptr error) {
    if (!error) {
        return fail("task failed without an error");
    }
    return owner_->task->finish(TaskState::failed, {}, std::move(error));
}

bool TaskControl::fail(const std::string& message) {
    return owner_->task->finish(TaskState::failed, {},
                                std::make_exception_ptr(TaskError(TaskErrc::failed, message)));
}

bool TaskControl::finish_cancelled() {
    return owner_->task->finish(TaskState::cancelled, {}, nullptr);
}

TaskControl make_task(std::string title) {
    return TaskControl(
        std::make_shared<TaskControl::Owner>(std::shared_ptr<Task>(new Task(std::move(title)))));
}

}  // namespace fem::task
