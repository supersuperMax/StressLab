#pragma once

#include "fem/core/concurrent_signal.h"
#include "fem/core/job/executor.h"
#include "fem/core/job/job_spec.h"
#include "fem/core/signal.h"
#include "fem/core/task/task.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fem::job {

class JobManager;

using JobId = std::uint64_t;

/// A line a program wrote, without the line break (`\n` or `\r\n`). Bytes as the program wrote
/// them: UTF-8 is expected, nothing is converted. A line longer than `max_line_bytes` is split.
struct LogLine {
    static constexpr std::size_t max_line_bytes = 64 * 1024;

    OutputStream stream = OutputStream::standard_output;
    std::string text;

    friend bool operator==(const LogLine&, const LogLine&) = default;
};

/// Called for every line of a job, on a thread of the job, one line at a time. A solver adapter
/// reads its progress from the lines and reports it to the task (`parse_progress` of
/// docs/architecture.md §6): the Job Manager knows nothing of the formats of solvers.
using LineHandler = std::function<void(const LogLine& line, task::TaskControl& control)>;

struct SubmitOptions {
    /// The executor to run the job; empty for the default one (the first added).
    std::string executor;
    LineHandler on_line;
};

struct JobManagerOptions {
    /// How many last lines of its output a job keeps for `Job::log`.
    std::size_t log_tail_lines = 10000;
};

/// A job submitted to the Job Manager. Its progress, messages, cancellation and result are those
/// of its task, which a command can return to the shell (`CommandResult::task`).
///
/// The task succeeds with `{"exitCode": 0}` when the program exits with 0 and wrote every output
/// of the spec. It fails with a JobError when the program cannot start (`launch_failed`,
/// `missing_input`), exits otherwise (`exit_code`) or misses an output (`missing_output`). It is
/// cancelled when cancelled before it ends.
class Job {
    struct Key {
        explicit Key() = default;
    };

public:
    using LogListener = std::function<void(const LogLine&)>;

    Job(Key, JobId id, JobSpec spec, std::string executor, task::TaskControl control,
        LineHandler on_line, std::size_t log_tail_lines);
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    Job(Job&&) = delete;
    Job& operator=(Job&&) = delete;
    ~Job() = default;

    /// Unique within its Job Manager, in the order of submission.
    [[nodiscard]] JobId id() const noexcept { return id_; }
    [[nodiscard]] const JobSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] const std::string& executor() const noexcept { return executor_; }
    [[nodiscard]] const std::shared_ptr<task::Task>& task() const noexcept { return task_; }
    [[nodiscard]] task::TaskState state() const { return task_->state(); }
    /// Cancels the task: a queued job does not start, a running one is stopped with every process
    /// it started.
    bool cancel() { return task_->cancel(); }

    /// How the program ended; empty until it has ended or if it never started.
    [[nodiscard]] std::optional<ExitStatus> exit_status() const;
    /// The last lines of the output, oldest first.
    [[nodiscard]] std::vector<LogLine> log() const;
    /// Calls `listener` for every new line, on a thread of the job (see `ConcurrentSignal`).
    [[nodiscard]] Subscription subscribe_log(LogListener listener);

private:
    friend class JobManager;

    /// Runs the program to its end on the calling thread and finishes the task.
    void run(Executor& executor);
    void request_stop();
    void finish(const std::optional<ExitStatus>& status, std::exception_ptr wait_error);

    // Output, under `output_mutex_`.
    void on_output(OutputStream stream, std::string_view bytes);
    void append(OutputStream stream, std::string_view bytes);
    void emit_line(OutputStream stream, std::string_view text);
    void flush_output();

    const JobId id_;
    const JobSpec spec_;
    const std::string executor_;
    task::TaskControl control_;
    const std::shared_ptr<task::Task> task_;
    const LineHandler on_line_;
    const std::size_t log_tail_lines_;

    mutable std::mutex mutex_;
    std::unique_ptr<Execution> execution_;
    bool stop_requested_ = false;
    std::optional<ExitStatus> exit_status_;
    std::deque<LogLine> log_;

    // Taken while output is split into lines and handed out, so lines of the two streams reach
    // `on_line` and listeners one at a time.
    std::mutex output_mutex_;
    std::string pending_[2];  // The unfinished line of each stream.
    bool line_handler_failed_ = false;
    ConcurrentSignal<const LogLine&> line_written_;
};

/// Keeps the queue of jobs and runs them through executors (docs/architecture.md §6): an adapter
/// says what to run (`JobSpec`), the Job Manager decides where. Every executor has its own queue
/// in submission order and runs at most `max_parallel` jobs at once.
///
/// All members may be called on any thread. Each running job has a thread of its own, on which its
/// task changes and its lines are handed out.
class JobManager {
public:
    using JobListener = std::function<void(const std::shared_ptr<Job>&)>;

    explicit JobManager(JobManagerOptions options = {});
    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;
    JobManager(JobManager&&) = delete;
    JobManager& operator=(JobManager&&) = delete;
    /// Cancels every job and waits until the running ones have stopped.
    ~JobManager();

    /// Adds a place to run jobs. The first one added is the default. Throws
    /// JobError(invalid_argument) for a second executor with the same id or `max_parallel` = 0.
    void add_executor(std::shared_ptr<Executor> executor, std::size_t max_parallel = 1);
    /// Ids of the executors, in the order they were added.
    [[nodiscard]] std::vector<std::string> executors() const;

    /// Queues a job. Throws JobError(unknown_executor) if there is no such executor (or none at
    /// all). Problems of the spec itself (no program, no input) fail the job's task instead.
    std::shared_ptr<Job> submit(JobSpec spec, SubmitOptions options = {});

    /// Every job submitted and not removed, in submission order.
    [[nodiscard]] std::vector<std::shared_ptr<Job>> jobs() const;
    /// Forgets finished jobs, e.g. when the user clears the list.
    void remove_finished();

    /// Calls `listener` with every new job, on the thread that submitted it.
    [[nodiscard]] Subscription on_submitted(JobListener listener);

private:
    struct Queue {
        std::shared_ptr<Executor> executor;
        std::size_t max_parallel = 1;
        std::size_t running = 0;
        std::deque<std::shared_ptr<Job>> waiting;
    };

    void dispatch(Queue& queue);
    void run(std::shared_ptr<Job> job, Queue& queue);
    void reap_finished_threads(std::unique_lock<std::mutex>& lock);

    const JobManagerOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable thread_finished_;
    std::vector<std::string> executor_order_;
    std::map<std::string, Queue, std::less<>> queues_;
    std::vector<std::shared_ptr<Job>> jobs_;
    JobId next_id_ = 1;
    bool stopping_ = false;
    std::map<JobId, std::thread> running_threads_;
    std::vector<std::thread> finished_threads_;
    ConcurrentSignal<const std::shared_ptr<Job>&> submitted_;
};

}  // namespace fem::job
