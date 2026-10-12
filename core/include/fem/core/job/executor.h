#pragma once

#include "fem/core/job/job_spec.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace fem::job {

enum class OutputStream : std::uint8_t { standard_output, standard_error };

[[nodiscard]] std::string_view to_string(OutputStream stream) noexcept;

/// How a program ended: its exit code, or the signal that killed it (POSIX).
struct ExitStatus {
    std::optional<int> exit_code;
    std::optional<int> signal;

    [[nodiscard]] bool succeeded() const noexcept { return exit_code == 0; }

    friend bool operator==(const ExitStatus&, const ExitStatus&) = default;
};

/// Receives what a program writes, in pieces of any size (not lines). Calls for one stream come
/// one after another; the two streams may be delivered on different threads at the same time.
using OutputSink = std::function<void(OutputStream stream, std::string_view bytes)>;

/// A program started by an executor.
class Execution {
public:
    virtual ~Execution() = default;

    /// Asks to stop the program and every process it started. Returns at once; may be called on
    /// any thread, any number of times, before or during `wait`.
    virtual void cancel() noexcept = 0;
    /// Blocks until the program has ended and all its output has been given to the sink; the
    /// sink is not called after it returns. Called once.
    virtual ExitStatus wait() = 0;

protected:
    Execution() = default;
    Execution(const Execution&) = default;
    Execution(Execution&&) = default;
    Execution& operator=(const Execution&) = default;
    Execution& operator=(Execution&&) = default;
};

/// Where jobs run: this computer (`LocalExecutor`) now; a remote computer over SSH, a cluster
/// queue (Slurm, PBS) or a cloud later. Adapters only describe a job (`JobSpec`); the Job Manager
/// picks the executor, so a new executor needs no change in adapters (CLAUDE.md, rule 8).
class Executor {
public:
    virtual ~Executor() = default;

    /// "local", "ssh:cluster1", …; unique within a Job Manager.
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    /// Starts the job, or submits it where it runs. Throws JobError(launch_failed or
    /// missing_input) if it cannot start. `sink` receives the output until `wait` returns.
    [[nodiscard]] virtual std::unique_ptr<Execution> start(const JobSpec& spec,
                                                           OutputSink sink) = 0;

protected:
    Executor() = default;
    Executor(const Executor&) = default;
    Executor(Executor&&) = default;
    Executor& operator=(const Executor&) = default;
    Executor& operator=(Executor&&) = default;
};

}  // namespace fem::job
