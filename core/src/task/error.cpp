#include "fem/core/task/error.h"

namespace fem::task {

std::string_view to_string(TaskErrc code) noexcept {
    switch (code) {
        case TaskErrc::cancelled:
            return "cancelled";
        case TaskErrc::abandoned:
            return "abandoned";
        case TaskErrc::not_finished:
            return "not_finished";
        case TaskErrc::failed:
            return "failed";
    }
    return "unknown";
}

TaskError::TaskError(TaskErrc code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

TaskCancelled::TaskCancelled() : TaskError(TaskErrc::cancelled, "task was cancelled") {}

}  // namespace fem::task
