// The context keys of the jobs (`job.running`, `job.active`): set on the thread of the model from
// changes that come on worker threads.

#include "fem/core/job/job_keys.h"

#include "fem/core/context/context_keys.h"
#include "fem/core/context/value.h"
#include "fem/core/context/when.h"
#include "fem/core/job/executor.h"
#include "fem/core/job/job_manager.h"
#include "fem/core/job/job_spec.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using fem::context::ContextKeys;
using fem::context::ContextValue;
using fem::job::Execution;
using fem::job::ExitStatus;
using fem::job::Job;
using fem::job::JobKeys;
using fem::job::JobManager;
using fem::job::JobSpec;
using fem::job::OutputSink;
namespace job_keys = fem::job::job_keys;

constexpr std::chrono::seconds timeout{10};

/// Programs that run until the test lets them end.
class GateExecutor final : public fem::job::Executor {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "local"; }

    std::unique_ptr<Execution> start(const JobSpec& /*spec*/, OutputSink /*sink*/) override {
        return std::make_unique<Gated>(*this);
    }

    /// Lets every program end with exit code 0.
    void open() {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }

private:
    class Gated final : public Execution {
    public:
        explicit Gated(GateExecutor& gate) : gate_(gate) {}
        void cancel() noexcept override {
            {
                const std::lock_guard lock(gate_.mutex_);
                cancelled_ = true;
            }
            gate_.changed_.notify_all();
        }
        ExitStatus wait() override {
            std::unique_lock lock(gate_.mutex_);
            gate_.changed_.wait(lock, [this] { return gate_.open_ || cancelled_; });
            return cancelled_ ? ExitStatus{std::nullopt, 15} : ExitStatus{0, std::nullopt};
        }

    private:
        GateExecutor& gate_;
        bool cancelled_ = false;  // Under the mutex of the gate.
    };

    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

/// Work posted from any thread, run by the test as if by the event loop of the thread of the model.
class PostQueue {
public:
    JobKeys::Post post() {
        return [this](std::function<void()> work) {
            const std::lock_guard lock(mutex_);
            work_.push_back(std::move(work));
        };
    }
    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return work_.size();
    }
    void drain() {
        std::vector<std::function<void()>> work;
        {
            const std::lock_guard lock(mutex_);
            work.swap(work_);
        }
        for (const auto& item : work) {
            item();
        }
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::function<void()>> work_;
};

JobSpec spec() {
    JobSpec result;
    result.program = "solver";
    return result;
}

class JobKeysTest : public ::testing::Test {
protected:
    JobKeysTest() { jobs_.add_executor(gate_); }

    [[nodiscard]] bool running() const { return keys_.value(job_keys::running) == true; }
    [[nodiscard]] double active() const { return keys_.value(job_keys::active).as_number(); }

    /// Waits for a job to finish, then runs what was posted meanwhile. The tracker posts its
    /// update from the thread of the task, which may be after waiters wake: wait for the post too.
    void finish(const std::shared_ptr<Job>& job) {
        ASSERT_TRUE(job->task()->wait_for(timeout));
        wait_for_post();
        queue_.drain();
    }

    /// Waits until something is posted. A job starts on its worker thread before `submit`
    /// returns, so an event of its task may ask for the update first: it is posted from that
    /// thread, maybe a moment after `submit` returns.
    void wait_for_post() {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (queue_.size() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    ContextKeys keys_;
    PostQueue queue_;
    // Before the manager: the manager stops its threads first.
    std::shared_ptr<GateExecutor> gate_ = std::make_shared<GateExecutor>();
    JobManager jobs_;
};

TEST_F(JobKeysTest, DeclaresTheKeysOfTheCore) {
    const JobKeys tracker(jobs_, keys_, queue_.post());
    ASSERT_NE(keys_.find(job_keys::running), nullptr);
    EXPECT_EQ(keys_.find(job_keys::running)->owner, "fem.core");
    EXPECT_EQ(keys_.find(job_keys::running)->type, ContextValue::Kind::boolean);
    EXPECT_EQ(keys_.find(job_keys::active)->type, ContextValue::Kind::number);
    EXPECT_FALSE(running());
    EXPECT_EQ(active(), 0.0);
}

TEST_F(JobKeysTest, FollowJobsOnTheThreadOfTheModel) {
    const JobKeys tracker(jobs_, keys_, queue_.post());
    const std::shared_ptr<Job> first = jobs_.submit(spec());
    const std::shared_ptr<Job> second = jobs_.submit(spec());
    // Nothing changes until the posted work runs.
    EXPECT_FALSE(running());
    wait_for_post();
    queue_.drain();
    EXPECT_TRUE(running());
    EXPECT_EQ(active(), 2.0);

    // The second waits in the queue (one job at a time); cancelling it leaves the first.
    EXPECT_TRUE(second->task()->cancel());
    finish(second);
    EXPECT_TRUE(running());
    EXPECT_EQ(active(), 1.0);

    gate_->open();
    finish(first);
    EXPECT_FALSE(running());
    EXPECT_EQ(active(), 0.0);
}

TEST_F(JobKeysTest, AWhenClauseSeesTheJobs) {
    const JobKeys tracker(jobs_, keys_, queue_.post());
    std::vector<bool> seen;
    const fem::Subscription watch = keys_.watch(fem::context::WhenExpression::parse("job.running"),
                                                [&](bool on) { seen.push_back(on); });
    const std::shared_ptr<Job> job = jobs_.submit(spec());
    wait_for_post();
    queue_.drain();
    gate_->open();
    finish(job);
    EXPECT_EQ(seen, (std::vector<bool>{true, false}));
}

TEST_F(JobKeysTest, PostsOneUpdateAtATime) {
    const JobKeys tracker(jobs_, keys_, queue_.post());
    for (int i = 0; i < 5; ++i) {
        (void)jobs_.submit(spec());
    }
    wait_for_post();
    EXPECT_EQ(queue_.size(), 1U);
    queue_.drain();
    EXPECT_EQ(active(), 5.0);
    // A change after the update asks for another one.
    jobs_.jobs().back()->task()->cancel();
    wait_for_post();
    EXPECT_EQ(queue_.size(), 1U);
}

TEST_F(JobKeysTest, CountsJobsSubmittedBeforeIt) {
    const std::shared_ptr<Job> job = jobs_.submit(spec());
    const JobKeys tracker(jobs_, keys_, queue_.post());
    EXPECT_TRUE(running());
    gate_->open();
    finish(job);
    EXPECT_FALSE(running());
}

TEST_F(JobKeysTest, WorkPostedBeforeItIsDestroyedDoesNothing) {
    std::optional<JobKeys> tracker;
    tracker.emplace(jobs_, keys_, queue_.post());
    (void)jobs_.submit(spec());
    wait_for_post();
    ASSERT_EQ(queue_.size(), 1U);
    tracker.reset();
    queue_.drain();
    // The keys stay, with the values they had.
    EXPECT_FALSE(running());
    EXPECT_NE(keys_.find(job_keys::running), nullptr);
    // A new one picks the jobs up.
    tracker.emplace(jobs_, keys_, queue_.post());
    EXPECT_TRUE(running());
    gate_->open();
}

TEST_F(JobKeysTest, NeedsAWayToPost) {
    EXPECT_THROW(JobKeys(jobs_, keys_, nullptr), std::invalid_argument);
}

}  // namespace
