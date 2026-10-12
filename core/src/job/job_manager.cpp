#include "fem/core/job/job_manager.h"

#include "fem/core/command/value.h"
#include "fem/core/job/error.h"

#include "process.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <system_error>
#include <utility>

namespace fem::job {

namespace {

std::size_t index_of(OutputStream stream) {
    return stream == OutputStream::standard_output ? 0 : 1;
}

/// Where to cut a too long line: at the limit, moved back to the start of a UTF-8 character.
std::size_t cut_position(std::string_view text) {
    std::size_t cut = LogLine::max_line_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
        --cut;
    }
    // Not UTF-8 at all: cut at the limit anyway.
    return cut == 0 ? LogLine::max_line_bytes : cut;
}

std::string describe(const ExitStatus& status) {
    if (status.exit_code) {
        return "exited with code " + std::to_string(*status.exit_code);
    }
    if (status.signal) {
        return "was killed by signal " + std::to_string(*status.signal);
    }
    return "ended without an exit status";
}

}  // namespace

Job::Job(Key, JobId id, JobSpec spec, std::string executor, task::TaskControl control,
         LineHandler on_line, std::size_t log_tail_lines)
    : id_(id),
      spec_(std::move(spec)),
      executor_(std::move(executor)),
      control_(std::move(control)),
      task_(control_.task()),
      on_line_(std::move(on_line)),
      log_tail_lines_(log_tail_lines) {}

std::optional<ExitStatus> Job::exit_status() const {
    const std::lock_guard lock(mutex_);
    return exit_status_;
}

std::vector<LogLine> Job::log() const {
    const std::lock_guard lock(mutex_);
    return {log_.begin(), log_.end()};
}

Subscription Job::subscribe_log(LogListener listener) {
    return line_written_.subscribe(std::move(listener));
}

void Job::run(Executor& executor) {
    if (!control_.start()) {
        return;  // Cancelled while it waited in the queue.
    }
    std::unique_ptr<Execution> execution;
    try {
        execution = executor.start(spec_, [this](OutputStream stream, std::string_view bytes) {
            on_output(stream, bytes);
        });
    } catch (...) {
        control_.fail(std::current_exception());
        return;
    }
    Execution* running = execution.get();
    {
        const std::lock_guard lock(mutex_);
        execution_ = std::move(execution);
        // Asked to stop between the start of the task and now: `request_stop` had nothing to stop.
        if (stop_requested_) {
            running->cancel();
        }
    }
    std::optional<ExitStatus> status;
    std::exception_ptr wait_error;
    try {
        status = running->wait();
    } catch (...) {
        wait_error = std::current_exception();
    }
    flush_output();
    {
        const std::lock_guard lock(mutex_);
        exit_status_ = status;
        execution = std::move(execution_);
    }
    execution.reset();  // Not under the lock: it may wait for threads of the executor.
    finish(status, wait_error);
}

void Job::request_stop() {
    const std::lock_guard lock(mutex_);
    stop_requested_ = true;
    if (execution_) {
        execution_->cancel();
    }
}

void Job::finish(const std::optional<ExitStatus>& status, std::exception_ptr wait_error) {
    if (wait_error) {
        control_.fail(wait_error);
        return;
    }
    const std::string program = detail::to_utf8(spec_.program.filename());
    if (!status->succeeded()) {
        if (control_.is_cancel_requested()) {
            control_.finish_cancelled();
        } else {
            control_.fail(std::make_exception_ptr(
                JobError(JobErrc::exit_code, "program '" + program + "' " + describe(*status))));
        }
        return;
    }
    // A program that succeeded keeps its results even if it was asked to stop too late.
    for (const std::filesystem::path& output : spec_.outputs) {
        std::error_code error;
        const std::filesystem::path file = resolve_in_workdir(spec_, output);
        if (!std::filesystem::exists(file, error)) {
            control_.fail(std::make_exception_ptr(
                JobError(JobErrc::missing_output, "program '" + program +
                                                      "' did not write its output file '" +
                                                      detail::to_utf8(file) + "'")));
            return;
        }
    }
    control_.succeed(command::Value::Dict{{"exitCode", *status->exit_code}});
}

void Job::on_output(OutputStream stream, std::string_view bytes) {
    const std::lock_guard lock(output_mutex_);
    while (!bytes.empty()) {
        const std::size_t end = bytes.find('\n');
        if (end == std::string_view::npos) {
            append(stream, bytes);
            return;
        }
        append(stream, bytes.substr(0, end));
        std::string& pending = pending_[index_of(stream)];
        if (pending.ends_with('\r')) {
            pending.pop_back();
        }
        const std::string line = std::exchange(pending, {});
        emit_line(stream, line);
        bytes.remove_prefix(end + 1);
    }
}

void Job::append(OutputStream stream, std::string_view bytes) {
    std::string& pending = pending_[index_of(stream)];
    pending += bytes;
    while (pending.size() > LogLine::max_line_bytes) {
        const std::size_t cut = cut_position(pending);
        const std::string line = pending.substr(0, cut);
        pending.erase(0, cut);
        emit_line(stream, line);
    }
}

void Job::emit_line(OutputStream stream, std::string_view text) {
    const LogLine line{stream, std::string(text)};
    if (log_tail_lines_ > 0) {
        const std::lock_guard lock(mutex_);
        log_.push_back(line);
        if (log_.size() > log_tail_lines_) {
            log_.pop_front();
        }
    }
    if (on_line_) {
        // A broken parser of an adapter must not stop the solver; it is reported once.
        std::string problem;
        try {
            on_line_(line, control_);
        } catch (const std::exception& error) {
            problem = error.what();
        } catch (...) {
            problem = "unknown error";
        }
        if (!problem.empty() && !line_handler_failed_) {
            line_handler_failed_ = true;
            control_.message("reading the output of the job failed: " + problem,
                             task::MessageSeverity::warning);
        }
    }
    line_written_.emit(line);
}

void Job::flush_output() {
    const std::lock_guard lock(output_mutex_);
    for (const OutputStream stream :
         {OutputStream::standard_output, OutputStream::standard_error}) {
        std::string& pending = pending_[index_of(stream)];
        if (pending.ends_with('\r')) {
            pending.pop_back();
        }
        if (!pending.empty()) {
            const std::string line = std::exchange(pending, {});
            emit_line(stream, line);
        }
    }
}

JobManager::JobManager(JobManagerOptions options) : options_(options) {}

JobManager::~JobManager() {
    std::vector<std::shared_ptr<Job>> jobs;
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        jobs = jobs_;
    }
    for (const std::shared_ptr<Job>& job : jobs) {
        job->cancel();
    }
    std::unique_lock lock(mutex_);
    while (!running_threads_.empty() || !finished_threads_.empty()) {
        if (finished_threads_.empty()) {
            thread_finished_.wait(lock);
        }
        reap_finished_threads(lock);
    }
}

void JobManager::add_executor(std::shared_ptr<Executor> executor, std::size_t max_parallel) {
    if (!executor) {
        throw JobError(JobErrc::invalid_argument, "executor is null");
    }
    std::string id(executor->id());
    if (max_parallel == 0) {
        throw JobError(JobErrc::invalid_argument,
                       "executor '" + id + "' must run at least one job at a time");
    }
    const std::lock_guard lock(mutex_);
    if (queues_.contains(id)) {
        throw JobError(JobErrc::invalid_argument, "executor '" + id + "' is added already");
    }
    Queue queue;
    queue.executor = std::move(executor);
    queue.max_parallel = max_parallel;
    queues_.emplace(id, std::move(queue));
    executor_order_.push_back(std::move(id));
}

std::vector<std::string> JobManager::executors() const {
    const std::lock_guard lock(mutex_);
    return executor_order_;
}

std::shared_ptr<Job> JobManager::submit(JobSpec spec, SubmitOptions options) {
    {
        std::unique_lock lock(mutex_);
        reap_finished_threads(lock);
    }
    std::shared_ptr<Job> job;
    {
        const std::lock_guard lock(mutex_);
        if (executor_order_.empty()) {
            throw JobError(JobErrc::unknown_executor, "no executor is added to the Job Manager");
        }
        const std::string& id =
            options.executor.empty() ? executor_order_.front() : options.executor;
        const auto queue = queues_.find(id);
        if (queue == queues_.end()) {
            throw JobError(JobErrc::unknown_executor, "no executor '" + id + "'");
        }
        std::string title =
            spec.title.empty() ? detail::to_utf8(spec.program.filename()) : spec.title;
        task::TaskControl control = task::make_task(std::move(title));
        job = std::make_shared<Job>(Job::Key{}, next_id_++, std::move(spec), queue->first, control,
                                    std::move(options.on_line), options_.log_tail_lines);
        control.on_cancel([weak = std::weak_ptr<Job>(job)] {
            if (const std::shared_ptr<Job> cancelled = weak.lock()) {
                cancelled->request_stop();
            }
        });
        jobs_.push_back(job);
        queue->second.waiting.push_back(job);
        dispatch(queue->second);
    }
    submitted_.emit(job);
    return job;
}

std::vector<std::shared_ptr<Job>> JobManager::jobs() const {
    const std::lock_guard lock(mutex_);
    return jobs_;
}

void JobManager::remove_finished() {
    const std::lock_guard lock(mutex_);
    std::erase_if(jobs_,
                  [](const std::shared_ptr<Job>& job) { return job->task()->is_finished(); });
}

Subscription JobManager::on_submitted(JobListener listener) {
    return submitted_.subscribe(std::move(listener));
}

void JobManager::dispatch(Queue& queue) {
    while (!stopping_ && queue.running < queue.max_parallel && !queue.waiting.empty()) {
        std::shared_ptr<Job> job = std::move(queue.waiting.front());
        queue.waiting.pop_front();
        if (job->task()->is_finished()) {
            continue;  // Cancelled while it waited.
        }
        ++queue.running;
        const JobId id = job->id();
        running_threads_.emplace(
            id, std::thread([this, job = std::move(job), &queue] { run(job, queue); }));
    }
}

void JobManager::run(std::shared_ptr<Job> job, Queue& queue) {
    job->run(*queue.executor);
    {
        const std::lock_guard lock(mutex_);
        --queue.running;
        const auto self = running_threads_.find(job->id());
        finished_threads_.push_back(std::move(self->second));
        running_threads_.erase(self);
        dispatch(queue);
    }
    thread_finished_.notify_all();
}

void JobManager::reap_finished_threads(std::unique_lock<std::mutex>& lock) {
    std::vector<std::thread> finished = std::exchange(finished_threads_, {});
    if (finished.empty()) {
        return;
    }
    lock.unlock();
    for (std::thread& thread : finished) {
        thread.join();
    }
    lock.lock();
}

}  // namespace fem::job
