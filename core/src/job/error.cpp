#include "fem/core/job/error.h"

namespace fem::job {

std::string_view to_string(JobErrc code) noexcept {
    switch (code) {
        case JobErrc::launch_failed:
            return "launch_failed";
        case JobErrc::missing_input:
            return "missing_input";
        case JobErrc::missing_output:
            return "missing_output";
        case JobErrc::exit_code:
            return "exit_code";
        case JobErrc::unknown_executor:
            return "unknown_executor";
        case JobErrc::invalid_argument:
            return "invalid_argument";
    }
    return "unknown";
}

JobError::JobError(JobErrc code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

}  // namespace fem::job
