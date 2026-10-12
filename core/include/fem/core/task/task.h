#pragma once

#include "fem/core/command/value.h"
#include "fem/core/concurrent_signal.h"
#include "fem/core/signal.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fem::task {

class TaskControl;

/// Where a task is in its life. `succeeded`, `failed` and `cancelled` are final.
enum class TaskState : std::uint8_t { pending, running, succeeded, failed, cancelled };

[[nodiscard]] std::string_view to_string(TaskState state) noexcept;
[[nodiscard]] bool is_finished(TaskState state) noexcept;

enum class MessageSeverity : std::uint8_t { info, warning, error };

[[nodiscard]] std::string_view to_string(MessageSeverity severity) noexcept;

/// A line the work reports to the user while it runs, e.g. "Writing the input file".
struct TaskMessage {
    MessageSeverity severity = MessageSeverity::info;
    std::string text;  ///< UTF-8.

    friend bool operator==(const TaskMessage&, const TaskMessage&) = default;
};

/// What changed in a task. The listener reads the task for everything else.
struct TaskEvent {
    enum class Kind : std::uint8_t { state, progress, message };

    Kind kind = Kind::state;
    TaskState state = TaskState::pending;  ///< The state after the change.
    std::optional<double> progress;        ///< The progress after the change.
    TaskMessage message;                   ///< The new message, for `Kind::message`.
};

/// A long operation that runs in the background (docs/architecture.md §4.6, item 7): import,
/// meshing, a solver job. A command may return one in `CommandResult::task`; the shell shows it
/// in "Jobs" and does not wait for it.
///
/// This is the side of the caller: it watches the task, cancels it and takes its result. The side
/// of the work is `TaskControl`. All members may be called on any thread.
class Task final {
public:
    using Listener = std::function<void(const TaskEvent&)>;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) = delete;
    Task& operator=(Task&&) = delete;
    ~Task() = default;

    /// What the task does, for lists of tasks. UTF-8.
    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] TaskState state() const;
    [[nodiscard]] bool is_finished() const;
    /// From 0 to 1; empty while the work does not know how far it is.
    [[nodiscard]] std::optional<double> progress() const;
    /// Every message reported so far, oldest first.
    [[nodiscard]] std::vector<TaskMessage> messages() const;

    /// Asks the work to stop. A pending task is cancelled at once; a running one is cancelled when
    /// its work stops (`TaskControl::on_cancel` callbacks are called now, on this thread). Returns
    /// false if the task has finished or was asked before.
    bool cancel();
    [[nodiscard]] bool is_cancel_requested() const;

    /// Blocks until the task finishes.
    void wait() const;
    /// Blocks until the task finishes or the time runs out; true if it has finished.
    template <class Rep, class Period>
    bool wait_for(std::chrono::duration<Rep, Period> timeout) const {
        std::unique_lock lock(mutex_);
        return finished_.wait_for(lock, timeout, [this] { return task::is_finished(state_); });
    }

    /// The value of a succeeded task. Throws what the work threw for a failed one, TaskCancelled
    /// for a cancelled one and TaskError(not_finished) for one that is still going.
    [[nodiscard]] command::Value result() const;
    /// What a failed task threw; empty otherwise.
    [[nodiscard]] std::exception_ptr error() const;
    /// `what()` of the error of a failed task; empty otherwise.
    [[nodiscard]] std::string error_message() const;

    /// Calls `listener` after every change. It is called on the thread that made the change (a
    /// worker thread, as a rule) and not under any lock of the task, so it may read the task. A
    /// shell moves the notification to its own thread itself.
    [[nodiscard]] Subscription subscribe(Listener listener);

private:
    friend class TaskControl;
    friend TaskControl make_task(std::string title);

    explicit Task(std::string title);

    bool start();
    void set_progress(std::optional<double> progress);
    void add_message(TaskMessage message);
    void on_cancel(std::function<void()> callback);
    bool finish(TaskState state, command::Value value, std::exception_ptr error);

    const std::string title_;
    mutable std::mutex mutex_;
    mutable std::condition_variable finished_;
    TaskState state_ = TaskState::pending;
    std::optional<double> progress_;
    std::vector<TaskMessage> messages_;
    bool cancel_requested_ = false;
    std::vector<std::function<void()>> cancel_callbacks_;
    command::Value value_;
    std::exception_ptr error_;
    ConcurrentSignal<const TaskEvent&> changed_;
};

/// The side of the work: it starts the task, reports progress and messages, learns about
/// cancellation and finishes the task. Copies refer to the same task. When the last copy is
/// destroyed and the task has not finished, it fails with TaskError(abandoned), so a task never
/// hangs because the code running it is gone.
///
/// Calls after the task has finished are ignored (the finishing calls return false), so the work
/// does not need to check whether it was cancelled while pending.
class TaskControl {
public:
    [[nodiscard]] const std::shared_ptr<Task>& task() const noexcept;

    /// pending → running. False if the task has finished already (it was cancelled while
    /// pending): then the work should not run.
    bool start();

    /// Clamped to 0..1; NaN makes the progress indeterminate.
    void set_progress(double fraction);
    void set_indeterminate();
    void message(std::string text, MessageSeverity severity = MessageSeverity::info);

    [[nodiscard]] bool is_cancel_requested() const;
    /// Throws TaskCancelled if the task was asked to stop.
    void throw_if_cancel_requested() const;
    /// Calls `callback` once when the task is asked to stop, on the thread that asks; at once if
    /// it has been asked already. Forgotten when the task finishes. For work that waits on
    /// something it can interrupt, e.g. a process to kill.
    void on_cancel(std::function<void()> callback);

    bool succeed(command::Value value = {});
    bool fail(std::exception_ptr error);
    /// Fails with TaskError(failed, message).
    bool fail(const std::string& message);
    bool finish_cancelled();

private:
    friend TaskControl make_task(std::string title);

    struct Owner;
    explicit TaskControl(std::shared_ptr<Owner> owner);

    std::shared_ptr<Owner> owner_;
};

/// A new pending task, for work that runs it itself (a job, a thread of a module). Work that is
/// just a function goes to a `TaskPool` instead.
[[nodiscard]] TaskControl make_task(std::string title);

}  // namespace fem::task
