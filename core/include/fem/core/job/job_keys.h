#pragma once

#include "fem/core/context/context_keys.h"
#include "fem/core/job/job_manager.h"
#include "fem/core/signal.h"

#include <functional>
#include <memory>
#include <string_view>

namespace fem::job {

/// Context keys of the jobs, owned by "fem.core" (ADR 0005): `when` clauses of the shell read
/// them, e.g. a tab of a solver monitor shows while a job runs.
namespace job_keys {
/// A job is queued or running.
inline constexpr std::string_view running = "job.running";
/// How many jobs are queued or running (a number).
inline constexpr std::string_view active = "job.active";
}  // namespace job_keys

/// Keeps the keys of the jobs in step with a Job Manager.
///
/// Jobs change on their worker threads, while context keys change only on the thread of the
/// model; `post` moves the work there ("run this soon on the thread of the model"; the window
/// uses the event queue of Qt, the headless run the loop of the interpreter). It must be safe to
/// call on any thread. At most one update waits in it at a time, however often jobs change.
///
/// The keys are declared once per `ContextKeys` and keep their values when this object is
/// destroyed. Work posted before that does nothing when it runs later.
class JobKeys {
public:
    using Post = std::function<void(std::function<void()> work)>;

    /// Declares the keys in `keys` (unless they are declared) and sets them from `jobs`. Both
    /// must outlive this object; `post` must not be empty.
    JobKeys(JobManager& jobs, context::ContextKeys& keys, Post post);
    JobKeys(const JobKeys&) = delete;
    JobKeys& operator=(const JobKeys&) = delete;
    JobKeys(JobKeys&&) = delete;
    JobKeys& operator=(JobKeys&&) = delete;
    /// Stops listening to the jobs; when it returns, no listener of this object runs.
    ~JobKeys();

private:
    struct State;

    std::shared_ptr<State> state_;
    Subscription submitted_;
};

}  // namespace fem::job
