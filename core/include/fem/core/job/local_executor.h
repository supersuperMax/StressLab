#pragma once

#include "fem/core/job/executor.h"

#include <chrono>
#include <memory>
#include <string_view>

namespace fem::job {

struct LocalExecutorOptions {
    /// On POSIX, cancelling sends SIGTERM to the process group and SIGKILL after this time. On
    /// Windows processes are terminated at once.
    std::chrono::milliseconds terminate_grace{3000};
};

/// Runs jobs on this computer as child processes.
///
/// The program and every process it starts are kept together: a Windows Job Object or a POSIX
/// process group. Cancelling a job ends all of them. When the program itself ends, processes it
/// left behind are ended too, so none of them outlives the job or holds its output open. (On
/// POSIX a process that leaves the group itself, e.g. with `setsid`, is out of reach.)
///
/// The program gets an empty standard input; standard output and error are read separately.
/// Windows batch files (`.bat`, `.cmd`) are not started directly, because their arguments cannot
/// be passed safely; a spec runs `cmd.exe` explicitly if it has to.
class LocalExecutor final : public Executor {
public:
    static constexpr std::string_view executor_id = "local";

    explicit LocalExecutor(LocalExecutorOptions options = {});

    [[nodiscard]] std::string_view id() const noexcept override { return executor_id; }
    [[nodiscard]] std::unique_ptr<Execution> start(const JobSpec& spec, OutputSink sink) override;

private:
    LocalExecutorOptions options_;
};

}  // namespace fem::job
