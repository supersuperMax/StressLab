#include "fem/core/job/job_keys.h"

#include "fem/core/context/value.h"
#include "fem/core/task/task.h"

#include <atomic>
#include <cstddef>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fem::job {

namespace {

constexpr std::string_view owner = "fem.core";

}  // namespace

struct JobKeys::State {
    State(JobManager& jobs, context::ContextKeys& keys, Post post)
        : jobs(jobs), keys(keys), post(std::move(post)) {}

    /// Any thread: asks for an update on the thread of the model, unless one is waiting.
    void request() {
        if (pending.exchange(true)) {
            return;
        }
        post([weak = self] {
            if (const std::shared_ptr<State> state = weak.lock()) {
                state->update();
            }
        });
    }

    /// Any thread: follows the task of a new job.
    void follow(const std::shared_ptr<Job>& job) {
        Subscription subscription =
            job->task()->subscribe([this](const task::TaskEvent&) { request(); });
        const std::lock_guard lock(mutex);
        tasks.insert_or_assign(job->id(), std::move(subscription));
    }

    /// The thread of the model: sets the keys from the jobs.
    void update() {
        // Changes from now on ask for another update.
        pending.store(false);
        std::size_t active = 0;
        std::vector<JobId> finished;
        for (const std::shared_ptr<Job>& job : jobs.jobs()) {
            if (job->task()->is_finished()) {
                finished.push_back(job->id());
            } else {
                ++active;
            }
        }
        // A finished task does not change any more.
        std::vector<Subscription> done;
        {
            const std::lock_guard lock(mutex);
            for (const JobId id : finished) {
                if (const auto it = tasks.find(id); it != tasks.end()) {
                    done.push_back(std::move(it->second));
                    tasks.erase(it);
                }
            }
        }
        done.clear();  // Outside the lock: it waits for listeners that may be running.
        keys.set(
            {{std::string(job_keys::running), context::ContextValue(active > 0)},
             {std::string(job_keys::active), context::ContextValue(static_cast<double>(active))}});
    }

    JobManager& jobs;
    context::ContextKeys& keys;
    const Post post;
    std::weak_ptr<State> self;
    std::atomic<bool> pending{false};
    std::mutex mutex;
    std::map<JobId, Subscription> tasks;  // Under `mutex`.
};

JobKeys::JobKeys(JobManager& jobs, context::ContextKeys& keys, Post post) {
    if (!post) {
        throw std::invalid_argument("JobKeys needs a function to post work");
    }
    (void)keys.declare({.name = std::string(job_keys::running),
                        .owner = std::string(owner),
                        .type = context::ContextValue::Kind::boolean,
                        .initial = context::ContextValue(false)});
    (void)keys.declare({.name = std::string(job_keys::active),
                        .owner = std::string(owner),
                        .type = context::ContextValue::Kind::number,
                        .initial = context::ContextValue(0.0)});
    state_ = std::make_shared<State>(jobs, keys, std::move(post));
    state_->self = state_;
    submitted_ = jobs.on_submitted([state = state_.get()](const std::shared_ptr<Job>& job) {
        state->follow(job);
        state->request();
    });
    for (const std::shared_ptr<Job>& job : jobs.jobs()) {
        state_->follow(job);
    }
    state_->update();
}

JobKeys::~JobKeys() {
    submitted_.reset();
    std::map<JobId, Subscription> tasks;
    {
        const std::lock_guard lock(state_->mutex);
        tasks.swap(state_->tasks);
    }
    tasks.clear();
}

}  // namespace fem::job
