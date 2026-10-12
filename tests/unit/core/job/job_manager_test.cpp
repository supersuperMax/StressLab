// Job Manager with an executor of the test: the queue, cancellation, lines of the output and the
// choice of the executor, without real processes (task 1.6).

#include "fem/core/job/job_manager.h"

#include "fem/core/command/value.h"
#include "fem/core/job/error.h"
#include "fem/core/job/executor.h"
#include "fem/core/job/job_spec.h"
#include "fem/core/task/error.h"
#include "fem/core/task/task.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;
using fem::command::Value;
using fem::job::ExitStatus;
using fem::job::Job;
using fem::job::JobErrc;
using fem::job::JobError;
using fem::job::JobManager;
using fem::job::JobSpec;
using fem::job::LogLine;
using fem::job::OutputSink;
using fem::job::OutputStream;
using fem::job::SubmitOptions;
using fem::task::TaskControl;
using fem::task::TaskState;

constexpr auto timeout = 10s;
constexpr OutputStream out = OutputStream::standard_output;
constexpr OutputStream err = OutputStream::standard_error;

/// A program "running" in the test: the test writes its output and ends it.
class FakeProcess {
public:
    FakeProcess(JobSpec spec, OutputSink sink) : spec_(std::move(spec)), sink_(std::move(sink)) {}

    [[nodiscard]] const JobSpec& spec() const { return spec_; }
    void write(OutputStream stream, std::string_view bytes) { sink_(stream, bytes); }
    void exit(ExitStatus status) {
        {
            const std::lock_guard lock(mutex_);
            if (!status_) {
                status_ = status;
            }
        }
        changed_.notify_all();
    }
    void exit(int code) { exit(ExitStatus{code, std::nullopt}); }
    void cancel() {
        {
            const std::lock_guard lock(mutex_);
            ++cancel_calls_;
        }
        exit(ExitStatus{std::nullopt, 15});
    }
    [[nodiscard]] int cancel_calls() const {
        const std::lock_guard lock(mutex_);
        return cancel_calls_;
    }
    ExitStatus wait() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return status_.has_value(); });
        return *status_;
    }

private:
    JobSpec spec_;
    OutputSink sink_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<ExitStatus> status_;
    int cancel_calls_ = 0;
};

class FakeExecution final : public fem::job::Execution {
public:
    explicit FakeExecution(std::shared_ptr<FakeProcess> process) : process_(std::move(process)) {}
    void cancel() noexcept override { process_->cancel(); }
    ExitStatus wait() override { return process_->wait(); }

private:
    std::shared_ptr<FakeProcess> process_;
};

class FakeExecutor final : public fem::job::Executor {
public:
    explicit FakeExecutor(std::string id) : id_(std::move(id)) {}

    [[nodiscard]] std::string_view id() const noexcept override { return id_; }

    std::unique_ptr<fem::job::Execution> start(const JobSpec& spec, OutputSink sink) override {
        std::shared_ptr<FakeProcess> process;
        {
            const std::lock_guard lock(mutex_);
            if (fail_next_) {
                fail_next_ = false;
                throw JobError(JobErrc::missing_input, "input 'model.dat' does not exist");
            }
            process = std::make_shared<FakeProcess>(spec, std::move(sink));
            started_.push_back(process);
        }
        changed_.notify_all();
        return std::make_unique<FakeExecution>(process);
    }

    void fail_next() {
        const std::lock_guard lock(mutex_);
        fail_next_ = true;
    }

    /// The `index`-th process started, once it is.
    std::shared_ptr<FakeProcess> started(std::size_t index) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [&] { return started_.size() > index; })) {
            throw std::runtime_error("the process was not started");
        }
        return started_[index];
    }

    [[nodiscard]] std::size_t started_count() const {
        const std::lock_guard lock(mutex_);
        return started_.size();
    }

private:
    std::string id_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::shared_ptr<FakeProcess>> started_;
    bool fail_next_ = false;
};

JobSpec spec(fs::path program = "solver") {
    JobSpec result;
    result.program = std::move(program);
    result.args = {"-i", "job"};
    return result;
}

JobErrc job_error(const Job& job) {
    try {
        (void)job.task()->result();
    } catch (const JobError& error) {
        return error.code();
    }
    ADD_FAILURE() << "the job did not fail with a JobError";
    return JobErrc::invalid_argument;
}

struct Fixture {
    explicit Fixture(fem::job::JobManagerOptions options = {}) : manager(options) {
        manager.add_executor(executor);
    }

    // Declared before the manager, so the manager is destroyed first.
    std::shared_ptr<FakeExecutor> executor = std::make_shared<FakeExecutor>("local");
    JobManager manager;
};

TEST(JobManager, RunsJobAndSplitsOutputIntoLines) {
    Fixture f;
    const std::shared_ptr<Job> job = f.manager.submit(spec());
    EXPECT_EQ(job->id(), 1U);
    EXPECT_EQ(job->executor(), "local");
    const std::shared_ptr<FakeProcess> process = f.executor->started(0);
    EXPECT_EQ(process->spec(), spec());
    process->write(out, "hel");
    process->write(out, "lo\nwor");
    process->write(err, "warn\r");
    process->write(err, "\n");
    process->write(out, "ld\n\n");
    process->write(out, "tail without break");
    process->exit(0);
    ASSERT_TRUE(job->task()->wait_for(timeout));

    EXPECT_EQ(job->state(), TaskState::succeeded);
    EXPECT_EQ(job->task()->result(), Value(Value::Dict{{"exitCode", 0}}));
    EXPECT_EQ(job->exit_status(), (ExitStatus{0, std::nullopt}));
    const std::vector<LogLine> expected{
        {out, "hello"}, {err, "warn"}, {out, "world"}, {out, ""}, {out, "tail without break"},
    };
    EXPECT_EQ(job->log(), expected);
}

TEST(JobManager, TitleDefaultsToProgramName) {
    Fixture f;
    JobSpec named = spec();
    named.title = "Расчёт кронштейна";
    const std::shared_ptr<Job> a = f.manager.submit(spec(fs::path("bin") / "solver"));
    const std::shared_ptr<Job> b = f.manager.submit(named);
    EXPECT_EQ(a->task()->title(), "solver");
    EXPECT_EQ(b->task()->title(), "Расчёт кронштейна");
}

TEST(JobManager, LineHandlerReportsProgress) {
    Fixture f;
    SubmitOptions options;
    options.on_line = [](const LogLine& line, TaskControl& control) {
        constexpr std::string_view prefix = "PROGRESS ";
        if (line.text.starts_with(prefix)) {
            control.set_progress(std::stod(line.text.substr(prefix.size())));
        } else {
            control.message(line.text);
        }
    };
    const std::shared_ptr<Job> job = f.manager.submit(spec(), options);
    const std::shared_ptr<FakeProcess> process = f.executor->started(0);
    process->write(out, "PROGRESS 0.25\n");
    EXPECT_EQ(job->task()->progress(), 0.25);
    process->write(out, "Increment 2\nPROGRESS 0.5\n");
    EXPECT_EQ(job->task()->progress(), 0.5);
    EXPECT_EQ(job->task()->messages().size(), 1U);
    process->exit(0);
    ASSERT_TRUE(job->task()->wait_for(timeout));
}

TEST(JobManager, FailingLineHandlerDoesNotStopJob) {
    Fixture f;
    SubmitOptions options;
    options.on_line = [](const LogLine&, TaskControl&) { throw std::runtime_error("bad parser"); };
    const std::shared_ptr<Job> job = f.manager.submit(spec(), options);
    const std::shared_ptr<FakeProcess> process = f.executor->started(0);
    process->write(out, "a\nb\n");
    process->exit(0);
    ASSERT_TRUE(job->task()->wait_for(timeout));
    EXPECT_EQ(job->state(), TaskState::succeeded);
    ASSERT_EQ(job->task()->messages().size(), 1U) << "reported once";
    EXPECT_EQ(job->task()->messages()[0].severity, fem::task::MessageSeverity::warning);
    EXPECT_EQ(job->log().size(), 2U);
}

TEST(JobManager, NonZeroExitFailsJob) {
    Fixture f;
    const std::shared_ptr<Job> job = f.manager.submit(spec());
    f.executor->started(0)->exit(3);
    ASSERT_TRUE(job->task()->wait_for(timeout));
    EXPECT_EQ(job->state(), TaskState::failed);
    EXPECT_EQ(job_error(*job), JobErrc::exit_code);
    EXPECT_NE(job->task()->error_message().find("3"), std::string::npos);
    EXPECT_EQ(job->exit_status(), (ExitStatus{3, std::nullopt}));
}

TEST(JobManager, KilledBySignalFailsJob) {
    Fixture f;
    const std::shared_ptr<Job> job = f.manager.submit(spec());
    f.executor->started(0)->exit(ExitStatus{std::nullopt, 9});
    ASSERT_TRUE(job->task()->wait_for(timeout));
    EXPECT_EQ(job->state(), TaskState::failed);
    EXPECT_EQ(job_error(*job), JobErrc::exit_code);
    EXPECT_NE(job->task()->error_message().find("signal 9"), std::string::npos);
}

TEST(JobManager, LaunchFailureFailsJobAndQueueGoesOn) {
    Fixture f;
    f.executor->fail_next();
    const std::shared_ptr<Job> bad = f.manager.submit(spec());
    const std::shared_ptr<Job> good = f.manager.submit(spec());
    ASSERT_TRUE(bad->task()->wait_for(timeout));
    EXPECT_EQ(bad->state(), TaskState::failed);
    EXPECT_EQ(job_error(*bad), JobErrc::missing_input);
    EXPECT_EQ(bad->exit_status(), std::nullopt);
    f.executor->started(0)->exit(0);
    ASSERT_TRUE(good->task()->wait_for(timeout));
    EXPECT_EQ(good->state(), TaskState::succeeded);
}

TEST(JobManager, MissingOutputFailsJob) {
    const fs::path workdir = fs::temp_directory_path() / fs::path(u8"stresslab выходы");
    fs::remove_all(workdir);
    fs::create_directories(workdir);
    Fixture f;
    JobSpec with_output = spec();
    with_output.workdir = workdir;
    with_output.outputs = {fs::path(u8"результат.dat")};

    const std::shared_ptr<Job> missing = f.manager.submit(with_output);
    f.executor->started(0)->exit(0);
    ASSERT_TRUE(missing->task()->wait_for(timeout));
    EXPECT_EQ(missing->state(), TaskState::failed);
    EXPECT_EQ(job_error(*missing), JobErrc::missing_output);

    const std::shared_ptr<Job> present = f.manager.submit(with_output);
    std::ofstream(workdir / fs::path(u8"результат.dat")) << "ok\n";
    f.executor->started(1)->exit(0);
    ASSERT_TRUE(present->task()->wait_for(timeout));
    EXPECT_EQ(present->state(), TaskState::succeeded);
    fs::remove_all(workdir);
}

TEST(JobManager, QueueRunsOneJobAtATimeByDefault) {
    Fixture f;
    const std::shared_ptr<Job> first = f.manager.submit(spec());
    const std::shared_ptr<Job> second = f.manager.submit(spec());
    const std::shared_ptr<FakeProcess> p1 = f.executor->started(0);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(f.executor->started_count(), 1U);
    EXPECT_EQ(second->state(), TaskState::pending);
    p1->exit(0);
    f.executor->started(1)->exit(0);
    ASSERT_TRUE(second->task()->wait_for(timeout));
    EXPECT_EQ(first->state(), TaskState::succeeded);
    EXPECT_EQ(second->state(), TaskState::succeeded);
}

TEST(JobManager, RunsUpToMaxParallel) {
    JobManager manager;
    const auto executor = std::make_shared<FakeExecutor>("local");
    manager.add_executor(executor, 2);
    std::vector<std::shared_ptr<Job>> jobs;
    for (int i = 0; i < 3; ++i) {
        jobs.push_back(manager.submit(spec()));
    }
    executor->started(1);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(executor->started_count(), 2U);
    executor->started(0)->exit(0);
    executor->started(2)->exit(0);
    executor->started(1)->exit(0);
    for (const auto& job : jobs) {
        ASSERT_TRUE(job->task()->wait_for(timeout));
        EXPECT_EQ(job->state(), TaskState::succeeded);
    }
}

TEST(JobManager, CancelledWhileQueuedNeverStarts) {
    Fixture f;
    const std::shared_ptr<Job> first = f.manager.submit(spec());
    const std::shared_ptr<Job> second = f.manager.submit(spec());
    const std::shared_ptr<Job> third = f.manager.submit(spec());
    EXPECT_TRUE(second->cancel());
    EXPECT_EQ(second->state(), TaskState::cancelled);
    f.executor->started(0)->exit(0);
    f.executor->started(1)->exit(0);
    ASSERT_TRUE(third->task()->wait_for(timeout));
    EXPECT_EQ(f.executor->started_count(), 2U);
    EXPECT_EQ(second->exit_status(), std::nullopt);
}

TEST(JobManager, CancelRunningJobStopsExecution) {
    Fixture f;
    const std::shared_ptr<Job> job = f.manager.submit(spec());
    const std::shared_ptr<FakeProcess> process = f.executor->started(0);
    process->write(out, "Increment 1\n");
    // Cancelling through the task is the same: the shell cancels what a command returned.
    EXPECT_TRUE(job->task()->cancel());
    ASSERT_TRUE(job->task()->wait_for(timeout));
    EXPECT_EQ(job->state(), TaskState::cancelled);
    EXPECT_EQ(process->cancel_calls(), 1);
    EXPECT_EQ(job->exit_status(), (ExitStatus{std::nullopt, 15}));
    EXPECT_THROW((void)job->task()->result(), fem::task::TaskCancelled);
    EXPECT_EQ(job->log().size(), 1U);
}

TEST(JobManager, CancelledJobThatExitsWithZeroSucceeds) {
    // The program finished before it could be stopped: its results are kept.
    Fixture f;
    const std::shared_ptr<Job> job = f.manager.submit(spec());
    const std::shared_ptr<FakeProcess> process = f.executor->started(0);
    process->exit(0);
    job->cancel();
    ASSERT_TRUE(job->task()->wait_for(timeout));
    EXPECT_EQ(job->state(), TaskState::succeeded);
}

TEST(JobManager, ChoosesExecutorByIdWithoutChangingSpec) {
    JobManager manager;
    const auto local = std::make_shared<FakeExecutor>("local");
    const auto cluster = std::make_shared<FakeExecutor>("slurm:cluster");
    manager.add_executor(local);
    manager.add_executor(cluster, 4);
    EXPECT_EQ(manager.executors(), (std::vector<std::string>{"local", "slurm:cluster"}));

    SubmitOptions remote;
    remote.executor = "slurm:cluster";
    const std::shared_ptr<Job> there = manager.submit(spec(), remote);
    const std::shared_ptr<Job> here = manager.submit(spec());
    EXPECT_EQ(there->executor(), "slurm:cluster");
    EXPECT_EQ(here->executor(), "local");
    EXPECT_EQ(cluster->started(0)->spec(), local->started(0)->spec());
    cluster->started(0)->exit(0);
    local->started(0)->exit(0);
    ASSERT_TRUE(there->task()->wait_for(timeout));
    ASSERT_TRUE(here->task()->wait_for(timeout));
}

TEST(JobManager, RejectsUnknownExecutor) {
    JobManager empty;
    try {
        empty.submit(spec());
        FAIL() << "no executor";
    } catch (const JobError& error) {
        EXPECT_EQ(error.code(), JobErrc::unknown_executor);
    }
    Fixture f;
    SubmitOptions options;
    options.executor = "ssh:nowhere";
    try {
        f.manager.submit(spec(), options);
        FAIL() << "unknown executor";
    } catch (const JobError& error) {
        EXPECT_EQ(error.code(), JobErrc::unknown_executor);
    }
    EXPECT_TRUE(f.manager.jobs().empty());
}

TEST(JobManager, RejectsBadExecutors) {
    JobManager manager;
    manager.add_executor(std::make_shared<FakeExecutor>("local"));
    EXPECT_THROW(manager.add_executor(std::make_shared<FakeExecutor>("local")), JobError);
    EXPECT_THROW(manager.add_executor(std::make_shared<FakeExecutor>("other"), 0), JobError);
    EXPECT_THROW(manager.add_executor(nullptr), JobError);
}

TEST(JobManager, KeepsTailOfLog) {
    Fixture f({.log_tail_lines = 3});
    const std::shared_ptr<Job> job = f.manager.submit(spec());
    const std::shared_ptr<FakeProcess> process = f.executor->started(0);
    process->write(out, "1\n2\n3\n4\n5\n");
    process->exit(0);
    ASSERT_TRUE(job->task()->wait_for(timeout));
    EXPECT_EQ(job->log(), (std::vector<LogLine>{{out, "3"}, {out, "4"}, {out, "5"}}));
}

TEST(JobManager, SplitsTooLongLineAtCharacterBoundary) {
    Fixture f;
    const std::shared_ptr<Job> job = f.manager.submit(spec());
    const std::shared_ptr<FakeProcess> process = f.executor->started(0);
    // "я" is two bytes; one of them would end up past the limit.
    std::string text(LogLine::max_line_bytes - 1, 'x');
    text += "я";
    text += "tail\n";
    process->write(out, text);
    process->exit(0);
    ASSERT_TRUE(job->task()->wait_for(timeout));
    const std::vector<LogLine> log = job->log();
    ASSERT_EQ(log.size(), 2U);
    EXPECT_EQ(log[0].text, std::string(LogLine::max_line_bytes - 1, 'x'));
    EXPECT_EQ(log[1].text, "яtail");
}

TEST(JobManager, LogListenersAndSubmittedJobs) {
    Fixture f;
    std::vector<fem::job::JobId> submitted;
    const fem::Subscription on_submitted = f.manager.on_submitted(
        [&](const std::shared_ptr<Job>& job) { submitted.push_back(job->id()); });
    const std::shared_ptr<Job> first = f.manager.submit(spec());
    std::vector<std::string> lines;
    const fem::Subscription on_line =
        first->subscribe_log([&](const LogLine& line) { lines.push_back(line.text); });
    const std::shared_ptr<Job> second = f.manager.submit(spec());
    EXPECT_EQ(submitted, (std::vector<fem::job::JobId>{1, 2}));

    const std::shared_ptr<FakeProcess> process = f.executor->started(0);
    process->write(err, "Решение сошлось\n");
    process->exit(0);
    ASSERT_TRUE(first->task()->wait_for(timeout));
    EXPECT_EQ(lines, (std::vector<std::string>{"Решение сошлось"}));

    EXPECT_EQ(f.manager.jobs(), (std::vector<std::shared_ptr<Job>>{first, second}));
    f.manager.remove_finished();
    EXPECT_EQ(f.manager.jobs(), (std::vector<std::shared_ptr<Job>>{second}));
    f.executor->started(1)->exit(0);
    ASSERT_TRUE(second->task()->wait_for(timeout));
}

TEST(JobManager, DestructorCancelsEveryJob) {
    const auto executor = std::make_shared<FakeExecutor>("local");
    std::shared_ptr<Job> running;
    std::shared_ptr<Job> queued;
    {
        JobManager manager;
        manager.add_executor(executor);
        running = manager.submit(spec());
        queued = manager.submit(spec());
        executor->started(0);
    }
    EXPECT_EQ(running->state(), TaskState::cancelled);
    EXPECT_EQ(queued->state(), TaskState::cancelled);
    EXPECT_EQ(executor->started_count(), 1U);
}

TEST(JobErrors, HaveNames) {
    EXPECT_EQ(to_string(JobErrc::launch_failed), "launch_failed");
    EXPECT_EQ(to_string(JobErrc::missing_input), "missing_input");
    EXPECT_EQ(to_string(JobErrc::missing_output), "missing_output");
    EXPECT_EQ(to_string(JobErrc::exit_code), "exit_code");
    EXPECT_EQ(to_string(JobErrc::unknown_executor), "unknown_executor");
    EXPECT_EQ(to_string(JobErrc::invalid_argument), "invalid_argument");
    EXPECT_EQ(to_string(OutputStream::standard_error), "stderr");
}

TEST(JobSpec, ResolvesPathsInWorkdir) {
    JobSpec job;
    job.workdir = fs::path(u8"расчёт");
    EXPECT_EQ(fem::job::resolve_in_workdir(job, "a.dat"), fs::path(u8"расчёт") / "a.dat");
    const fs::path absolute = fs::temp_directory_path() / "b.dat";
    EXPECT_EQ(fem::job::resolve_in_workdir(job, absolute), absolute);
}

}  // namespace
