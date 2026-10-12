#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fem::job {

/// What went wrong with a job.
enum class JobErrc {
    launch_failed,     ///< The program could not be started (not found, no working folder, …).
    missing_input,     ///< An input file of the job does not exist.
    missing_output,    ///< The program succeeded, but an output file of the job does not exist.
    exit_code,         ///< The program exited with a code other than 0 or was killed by a signal.
    unknown_executor,  ///< No executor with this id is added to the Job Manager.
    invalid_argument,  ///< A wrong call, e.g. two executors with the same id.
};

[[nodiscard]] std::string_view to_string(JobErrc code) noexcept;

/// Error raised by the Job Manager and executors; a failed job's task throws it from `result()`.
/// The message is meant for developers and logs; text shown to users is produced from `code()`
/// through localization.
class JobError : public std::runtime_error {
public:
    JobError(JobErrc code, const std::string& message);

    [[nodiscard]] JobErrc code() const noexcept { return code_; }

private:
    JobErrc code_;
};

}  // namespace fem::job
