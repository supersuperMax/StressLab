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

/// A program started by `LocalExecutor::start_interactive`: the application writes to its
/// standard input. Used for programs that talk to the application while they run (the extension
/// host, process modules), which only run on this computer.
class InteractiveExecution : public Execution {
public:
    /// Writes all of `bytes` to the standard input of the program; blocks while the program does
    /// not read. Returns false if the program does not read its input any more (it has ended or
    /// closed it, or `close_input` was called): nothing is signalled to the application (no
    /// SIGPIPE). Calls from several threads are serialized.
    virtual bool write_input(std::string_view bytes) = 0;
    /// Closes the standard input: the program reads the end of the file. Waits for a write in
    /// progress (`cancel` ends one that blocks). May be called any number of times.
    virtual void close_input() noexcept = 0;

protected:
    InteractiveExecution() = default;
};

/// Runs jobs on this computer as child processes.
///
/// The program and every process it starts are kept together: a Windows Job Object or a POSIX
/// process group. Cancelling a job ends all of them. When the program itself ends, processes it
/// left behind are ended too, so none of them outlives the job or holds its output open. (On
/// POSIX a process that leaves the group itself, e.g. with `setsid`, is out of reach.)
///
/// The program gets an empty standard input (except with `start_interactive`); standard output
/// and error are read separately. Windows batch files (`.bat`, `.cmd`) are not started directly,
/// because their arguments cannot be passed safely; a spec runs `cmd.exe` explicitly if it has
/// to.
class LocalExecutor final : public Executor {
public:
    static constexpr std::string_view executor_id = "local";

    explicit LocalExecutor(LocalExecutorOptions options = {});

    [[nodiscard]] std::string_view id() const noexcept override { return executor_id; }
    [[nodiscard]] std::unique_ptr<Execution> start(const JobSpec& spec, OutputSink sink) override;
    /// Starts the program as `start` does, with a channel to its standard input. Throws
    /// JobError(launch_failed or missing_input).
    [[nodiscard]] std::unique_ptr<InteractiveExecution> start_interactive(const JobSpec& spec,
                                                                          OutputSink sink);

private:
    [[nodiscard]] std::unique_ptr<InteractiveExecution> start_process(const JobSpec& spec,
                                                                      OutputSink sink,
                                                                      bool with_input) const;

    LocalExecutorOptions options_;
};

}  // namespace fem::job
