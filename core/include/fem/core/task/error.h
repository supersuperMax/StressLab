#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fem::task {

/// Why a task did not give a result.
enum class TaskErrc {
    cancelled,     ///< The task was cancelled; `Task::result` throws a TaskCancelled.
    abandoned,     ///< The code running the task stopped without finishing it.
    not_finished,  ///< `Task::result` was called before the task finished.
    failed,        ///< The work reported a failure with a message only.
};

[[nodiscard]] std::string_view to_string(TaskErrc code) noexcept;

/// Error raised by tasks. The message is meant for developers and logs; text shown to users is
/// produced from `code()` through localization.
class TaskError : public std::runtime_error {
public:
    TaskError(TaskErrc code, const std::string& message);

    [[nodiscard]] TaskErrc code() const noexcept { return code_; }

private:
    TaskErrc code_;
};

/// Thrown by work that stops because it was asked to (`TaskControl::throw_if_cancel_requested`).
/// A task whose work throws it finishes as cancelled, not failed.
class TaskCancelled : public TaskError {
public:
    TaskCancelled();
};

}  // namespace fem::task
