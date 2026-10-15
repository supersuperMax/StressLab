#include "fem/core/extension/host_runtime.h"

#include "fem/core/command/error.h"
#include "fem/core/concurrent_signal.h"
#include "fem/core/context/when.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/ipc/json_rpc.h"
#include "fem/core/ipc/shared_memory.h"
#include "fem/core/job/error.h"
#include "fem/core/job/local_executor.h"
#include "fem/core/log/log.h"
#include "fem/core/product.h"
#include "fem/core/task/task.h"
#include "fem/core/version.h"

#include "files.h"
#include "host_data.h"
#include "json_value.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace fem::extension {

namespace {

using Clock = std::chrono::steady_clock;
using ipc::Json;
using ipc::Message;
using ipc::Response;
using ipc::RpcError;
namespace rpc_error = ipc::rpc_error;

/// The version of the protocol (docs/extensions/protocol.md).
constexpr int protocol_version = 1;
/// Lines of the error output of a module kept for the report when it stops.
constexpr std::size_t stderr_tail_lines = 20;

std::shared_ptr<spdlog::logger> extension_log() {
    return log::logger("extension");
}

/// An error answer for a request the application could not do.
class RequestError : public std::runtime_error {
public:
    RequestError(int code, const std::string& message) : std::runtime_error(message), code_(code) {}
    [[nodiscard]] int code() const noexcept { return code_; }

private:
    int code_;
};

[[noreturn]] void bad_params(const std::string& message) {
    throw RequestError(rpc_error::invalid_params, message);
}

const Json& param(const Json& params, std::string_view key) {
    if (!params.is_object()) {
        bad_params("params must be an object");
    }
    const auto it = params.find(key);
    if (it == params.end()) {
        bad_params("'" + std::string(key) + "' is missing");
    }
    return *it;
}

std::string string_param(const Json& params, std::string_view key) {
    const Json& value = param(params, key);
    if (!value.is_string()) {
        bad_params("'" + std::string(key) + "' must be a string");
    }
    return value.get<std::string>();
}

std::string optional_string(const Json& params, std::string_view key) {
    if (!params.is_object() || !params.contains(key) || params[key].is_null()) {
        return {};
    }
    return string_param(params, key);
}

std::int64_t integer_param(const Json& params, std::string_view key) {
    const Json& value = param(params, key);
    if (!value.is_number_integer()) {
        bad_params("'" + std::string(key) + "' must be an integer");
    }
    return value.get<std::int64_t>();
}

model::ObjectId object_param(const Json& value, std::string_view key) {
    if (!value.is_number_integer() || value.get<std::int64_t>() <= 0) {
        bad_params("'" + std::string(key) + "' must be an object id");
    }
    return model::ObjectId(value.get<std::uint64_t>());
}

command::Value::Dict dict_param(const Json& params, std::string_view key) {
    if (!params.is_object() || !params.contains(key) || params[key].is_null()) {
        return {};
    }
    command::Value value = detail::from_json(params[key]);
    if (value.kind() != command::Value::Kind::dict) {
        bad_params("'" + std::string(key) + "' must be an object");
    }
    return std::move(value.as_dict());
}

Json summary_json(const command::ModelChange& change) {
    const auto ids = [](const std::vector<model::ObjectId>& list) {
        Json json = Json::array();
        for (const model::ObjectId id : list) {
            json.push_back(id.value());
        }
        return json;
    };
    std::string_view cause = "execute";
    if (change.cause == command::ModelChange::Cause::undo) {
        cause = "undo";
    } else if (change.cause == command::ModelChange::Cause::redo) {
        cause = "redo";
    }
    return {{"cause", cause},
            {"label", change.label},
            {"created", ids(change.summary.created)},
            {"updated", ids(change.summary.updated)},
            {"removed", ids(change.summary.removed)},
            {"units", change.summary.units}};
}

std::optional<context::ContextValue::Kind> parse_key_type(std::string_view name) {
    using Kind = context::ContextValue::Kind;
    for (const Kind kind : {Kind::boolean, Kind::number, Kind::string, Kind::list}) {
        if (context::to_string(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string describe(const job::ExitStatus& status) {
    if (status.exit_code) {
        return "exit code " + std::to_string(*status.exit_code);
    }
    if (status.signal) {
        return "signal " + std::to_string(*status.signal);
    }
    return "no exit status";
}

}  // namespace

namespace detail {

// --- HostProcess ---------------------------------------------------------------------------------

/// A request or a notification of a module, for the thread of the model.
struct Inbound {
    Message message;
};

/// One running process of a module: its connection, its threads and the tasks it runs.
///
/// Threads: the reader threads of the executor take its output (`Peer::feed`); a writer thread
/// sends what is queued, so a module that stops reading does not block the application; a
/// monitor thread waits for the process to end and closes the connection then. Everything for
/// the thread of the model is queued in the inbox.
class HostProcess {
public:
    HostProcess(std::string id, std::function<void()> on_inbound)
        : id_(std::move(id)), on_inbound_(std::move(on_inbound)) {}
    HostProcess(const HostProcess&) = delete;
    HostProcess& operator=(const HostProcess&) = delete;
    HostProcess(HostProcess&&) = delete;
    HostProcess& operator=(HostProcess&&) = delete;
    ~HostProcess() { stop(); }

    /// Starts the program. Throws JobError.
    void start(const job::JobSpec& spec) {
        peer_ = std::make_unique<ipc::Peer>(
            [this](std::string frame) { return enqueue(std::move(frame)); },
            [this](Message message) { take(std::move(message)); });
        job::LocalExecutor executor;
        execution_ = executor.start_interactive(
            spec, [this](job::OutputStream stream, std::string_view bytes) {
                if (stream == job::OutputStream::standard_output) {
                    if (!peer_->feed(bytes) && !exited()) {
                        execution_->cancel();  // It broke the protocol.
                    }
                } else {
                    take_error_output(bytes);
                }
            });
        writer_ = std::thread([this] { write_loop(); });
        monitor_ = std::thread([this] { monitor(); });
    }

    /// Ends the process if it runs and waits for its threads. Idempotent.
    void stop() noexcept {
        if (execution_ && !exited()) {
            execution_->cancel();
        }
        if (monitor_.joinable()) {
            monitor_.join();
        }
        {
            const std::lock_guard lock(mutex_);
            writer_stop_ = true;
        }
        writable_.notify_all();
        if (writer_.joinable()) {
            writer_.join();
        }
    }

    /// Ends the process now; `reason` is what its stop is reported as.
    void kill(CrashReason reason) noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (reason == CrashReason::hung) {
                hung_ = true;
            }
        }
        if (execution_) {
            execution_->cancel();
        }
    }

    /// Asks the process to end by itself: its input is closed.
    void close_input() noexcept {
        {
            const std::lock_guard lock(mutex_);
            input_closing_ = true;
        }
        writable_.notify_all();
    }

    [[nodiscard]] bool exited() const {
        const std::lock_guard lock(mutex_);
        return exited_;
    }
    [[nodiscard]] bool hung() const {
        const std::lock_guard lock(mutex_);
        return hung_;
    }
    /// Waits until the process has ended; false if it has not within `timeout`.
    bool wait_exit(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return exited_; });
    }

    /// Exit status, the last lines of the error output.
    [[nodiscard]] std::string stop_details() const {
        const std::lock_guard lock(mutex_);
        std::string text = exited_ ? describe(exit_status_) : "still running";
        if (hung_) {
            text = "no answer, ended; " + text;
        }
        if (!stderr_tail_.empty()) {
            text += "; last error output:";
            for (const std::string& line : stderr_tail_) {
                text += "\n  " + line;
            }
        }
        return text;
    }

    // --- Sending ---------------------------------------------------------------------------------

    /// Sends a request; its answer is collected for `wait`. Returns the token to wait for.
    std::uint64_t request(std::string method, Json params) {
        std::uint64_t token = 0;
        {
            const std::lock_guard lock(mutex_);
            token = ++next_token_;
        }
        const bool command = method == "command/execute";
        peer_->request(std::move(method), std::move(params), [this, token, command](Response r) {
            if (command && r.ok() && r.result.is_object() && r.result.contains("task")) {
                start_task(r.result["task"]);
            }
            {
                const std::lock_guard lock(mutex_);
                responses_.emplace(token, std::move(r));
                ++activity_;
            }
            changed_.notify_all();
        });
        return token;
    }
    void notify(std::string method, Json params = nullptr) {
        peer_->notify(std::move(method), std::move(params));
    }
    void respond(const Message& request, Json result) {
        if (request.kind == Message::Kind::request) {
            peer_->respond(request.id, std::move(result));
        }
    }
    void respond_error(const Message& request, int code, const std::string& message) {
        if (request.kind == Message::Kind::request) {
            peer_->respond_error(request.id, RpcError{code, message, nullptr});
        }
    }

    /// Waits on the thread of the model for the answer to `token`. Requests of the module that
    /// come meanwhile go to `serve`. If the module sends nothing for `inactivity`, it is ended as
    /// hung. A stopped process answers with a `connection_lost` error.
    Response wait(std::uint64_t token, std::chrono::milliseconds inactivity,
                  const std::function<void(Message&)>& serve) {
        std::unique_lock lock(mutex_);
        // Answers to pings do not count: a module whose command loops forever still answers them.
        std::uint64_t seen = activity_;
        Clock::time_point since = Clock::now();
        for (;;) {
            if (activity_ != seen) {
                seen = activity_;
                since = Clock::now();
            }
            if (const auto it = responses_.find(token); it != responses_.end()) {
                Response response = std::move(it->second);
                responses_.erase(it);
                return response;
            }
            if (!inbox_.empty()) {
                Inbound inbound = std::move(inbox_.front());
                inbox_.pop_front();
                lock.unlock();
                serve(inbound.message);
                lock.lock();
                since = Clock::now();  // The time the application spent is not the module's.
                continue;
            }
            const Clock::time_point deadline = since + inactivity;
            if (!exited_ && !hung_ && Clock::now() >= deadline) {
                hung_ = true;
                lock.unlock();
                extension_log()->warn("module '{}' sent nothing for {} ms: ending it", id_,
                                      inactivity.count());
                execution_->cancel();
                lock.lock();
                continue;
            }
            if (exited_ || hung_) {
                changed_.wait(lock);
            } else {
                changed_.wait_until(lock, deadline);
            }
        }
    }

    /// The requests of the module waiting for the thread of the model.
    std::deque<Inbound> take_inbox() {
        const std::lock_guard lock(mutex_);
        std::deque<Inbound> inbox;
        inbox.swap(inbox_);
        return inbox;
    }

    // --- Tasks -----------------------------------------------------------------------------------

    /// The task a command of the module returned, created when its answer came.
    [[nodiscard]] std::shared_ptr<task::Task> task(const std::string& id) const {
        const std::lock_guard lock(tasks_mutex_);
        const auto it = tasks_.find(id);
        return it == tasks_.end() ? nullptr : it->second.task();
    }
    /// Fails the tasks still running: the module is gone.
    void fail_tasks(const std::string& reason) {
        std::map<std::string, task::TaskControl, std::less<>> tasks;
        {
            const std::lock_guard lock(tasks_mutex_);
            tasks.swap(tasks_);
            tasks_closed_ = true;
        }
        for (auto& [id, control] : tasks) {
            control.fail(
                std::make_exception_ptr(ExtensionError(ExtensionErrc::command_failed, reason)));
        }
    }

    // --- Shared memory ---------------------------------------------------------------------------

    /// Keeps a segment for the module until it says it has mapped it.
    void keep(ipc::SharedMemory segment) {
        const std::lock_guard lock(mutex_);
        segments_.emplace(segment.name(), std::move(segment));
    }
    [[nodiscard]] std::size_t segments() const {
        const std::lock_guard lock(mutex_);
        return segments_.size();
    }

    // --- Watchdog --------------------------------------------------------------------------------

    /// Called by the watchdog: sends a ping when it is due, ends the process when one is not
    /// answered in time.
    void check(Clock::time_point now, std::chrono::milliseconds interval,
               std::chrono::milliseconds timeout) {
        {
            const std::lock_guard lock(mutex_);
            if (exited_ || hung_) {
                return;
            }
            if (ping_sent_) {
                if (now - *ping_sent_ < timeout) {
                    return;
                }
                hung_ = true;
            } else if (now - last_pong_ < interval) {
                return;
            }
        }
        if (hung()) {
            extension_log()->warn("module '{}' does not answer pings: ending it", id_);
            execution_->cancel();
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            ping_sent_ = now;
        }
        peer_->request("$/ping", nullptr, [this](const Response&) {
            const std::lock_guard lock(mutex_);
            ping_sent_.reset();
            last_pong_ = Clock::now();
        });
    }

private:
    bool enqueue(std::string frame) {
        {
            const std::lock_guard lock(mutex_);
            if (writer_stop_ || input_closing_) {
                return false;
            }
            outbox_.push_back(std::move(frame));
        }
        writable_.notify_all();
        return true;
    }

    void write_loop() {
        std::unique_lock lock(mutex_);
        for (;;) {
            writable_.wait(lock,
                           [this] { return writer_stop_ || input_closing_ || !outbox_.empty(); });
            if (!outbox_.empty()) {
                std::string frame = std::move(outbox_.front());
                outbox_.pop_front();
                lock.unlock();
                const bool written = execution_->write_input(frame);
                lock.lock();
                if (!written) {
                    outbox_.clear();
                    writer_stop_ = true;
                }
                continue;
            }
            if (input_closing_) {
                lock.unlock();
                execution_->close_input();
                lock.lock();
                input_closing_ = false;
                writer_stop_ = true;
            }
            if (writer_stop_) {
                return;
            }
        }
    }

    void monitor() {
        job::ExitStatus status;
        try {
            status = execution_->wait();
        } catch (...) {
            // The status stays unknown.
        }
        {
            const std::lock_guard lock(mutex_);
            exit_status_ = status;
            exited_ = true;
            segments_.clear();
            if (!stderr_line_.empty()) {
                push_error_line(std::move(stderr_line_));
                stderr_line_.clear();
            }
        }
        peer_->close("module '" + id_ + "' stopped (" + describe(status) + ")");
        fail_tasks("module '" + id_ + "' stopped while the task ran");
        {
            const std::lock_guard lock(mutex_);
            writer_stop_ = true;
        }
        writable_.notify_all();
        changed_.notify_all();
        on_inbound_();
    }

    /// Requests and notifications of the module. Task and shared memory notifications are done
    /// here; the rest goes to the thread of the model.
    void take(Message message) {
        const std::string& method = message.method;
        if (method == "$/ping" && message.kind == Message::Kind::request) {
            peer_->respond(message.id, Json::object());
            return;
        }
        if (method.starts_with("task/") && message.kind == Message::Kind::notification) {
            task_notification(message);
            return;
        }
        if (method == "shm/release") {
            if (message.params.is_object() && message.params.contains("name") &&
                message.params["name"].is_string()) {
                const std::lock_guard lock(mutex_);
                segments_.erase(message.params["name"].get<std::string>());
            }
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            inbox_.push_back(Inbound{std::move(message)});
            ++activity_;
        }
        changed_.notify_all();
        on_inbound_();
    }

    void start_task(const Json& description) {
        if (!description.is_object() || !description.contains("id")) {
            return;
        }
        const Json& raw_id = description["id"];
        const std::string id = raw_id.is_string() ? raw_id.get<std::string>() : raw_id.dump();
        const std::string title = description.value("title", std::string());
        task::TaskControl control = task::make_task(title.empty() ? id_ : title);
        control.start();
        control.on_cancel([this, id] { notify("task/cancel", {{"task", id}}); });
        bool closed = false;
        {
            const std::lock_guard lock(tasks_mutex_);
            closed = tasks_closed_;
            if (!closed) {
                tasks_.insert_or_assign(id, control);
            }
        }
        if (closed) {
            control.fail(std::make_exception_ptr(
                ExtensionError(ExtensionErrc::command_failed, "module '" + id_ + "' stopped")));
        }
    }

    void task_notification(const Message& message) {
        const Json& params = message.params;
        if (!params.is_object() || !params.contains("task")) {
            return;
        }
        const Json& raw_id = params["task"];
        const std::string id = raw_id.is_string() ? raw_id.get<std::string>() : raw_id.dump();
        std::optional<task::TaskControl> control;
        {
            const std::lock_guard lock(tasks_mutex_);
            const auto it = tasks_.find(id);
            if (it == tasks_.end()) {
                return;  // Finished already, or never announced.
            }
            control = it->second;
            if (message.method == "task/finished") {
                tasks_.erase(it);
            }
        }
        try {
            if (message.method == "task/progress") {
                const Json& progress = params.value("progress", Json());
                if (progress.is_number()) {
                    control->set_progress(progress.get<double>());
                } else {
                    control->set_indeterminate();
                }
            } else if (message.method == "task/message") {
                const std::string severity = params.value("severity", std::string("info"));
                control->message(params.value("text", std::string()),
                                 severity == "error"     ? task::MessageSeverity::error
                                 : severity == "warning" ? task::MessageSeverity::warning
                                                         : task::MessageSeverity::info);
            } else if (message.method == "task/finished") {
                const std::string state = params.value("state", std::string());
                if (state == "succeeded") {
                    control->succeed(detail::from_json(params.value("value", Json())));
                } else if (state == "cancelled") {
                    control->finish_cancelled();
                } else {
                    control->fail(std::make_exception_ptr(ExtensionError(
                        ExtensionErrc::command_failed,
                        params.value("error", std::string("the task of the module failed")))));
                }
            }
        } catch (const std::exception& e) {
            control->fail(std::make_exception_ptr(ExtensionError(
                ExtensionErrc::command_failed, std::string("bad task result: ") + e.what())));
        }
    }

    void take_error_output(std::string_view bytes) {
        std::vector<std::string> lines;
        {
            const std::lock_guard lock(mutex_);
            stderr_line_ += bytes;
            std::size_t start = 0;
            for (std::size_t end = stderr_line_.find('\n'); end != std::string::npos;
                 end = stderr_line_.find('\n', start)) {
                std::string line = stderr_line_.substr(start, end - start);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                lines.push_back(line);
                push_error_line(std::move(line));
                start = end + 1;
            }
            stderr_line_.erase(0, start);
        }
        for (const std::string& line : lines) {
            extension_log()->info("[{}] {}", id_, line);
        }
    }

    void push_error_line(std::string line) {
        stderr_tail_.push_back(std::move(line));
        while (stderr_tail_.size() > stderr_tail_lines) {
            stderr_tail_.pop_front();
        }
    }

    const std::string id_;
    const std::function<void()> on_inbound_;
    std::unique_ptr<ipc::Peer> peer_;
    std::unique_ptr<job::InteractiveExecution> execution_;
    std::thread writer_;
    std::thread monitor_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;   // Responses, inbox, exit.
    std::condition_variable writable_;  // Outbox.
    std::deque<std::string> outbox_;
    bool writer_stop_ = false;
    bool input_closing_ = false;
    std::deque<Inbound> inbox_;
    std::uint64_t next_token_ = 0;
    std::map<std::uint64_t, Response> responses_;
    std::uint64_t activity_ = 0;  // Counts answers and requests of the module.
    bool exited_ = false;
    bool hung_ = false;
    job::ExitStatus exit_status_;
    std::optional<Clock::time_point> ping_sent_;
    Clock::time_point last_pong_ = Clock::now();
    std::map<std::string, ipc::SharedMemory, std::less<>> segments_;
    std::string stderr_line_;
    std::deque<std::string> stderr_tail_;

    mutable std::mutex tasks_mutex_;
    std::map<std::string, task::TaskControl, std::less<>> tasks_;
    bool tasks_closed_ = false;
};

class HostInstance;

}  // namespace detail

using detail::HostInstance;
using detail::HostProcess;
using detail::Inbound;

// --- State ---------------------------------------------------------------------------------------

/// What the runtime and its instances share; lives while either does.
struct HostRuntime::State {
    State(command::CommandBus& bus_, HostRuntimeOptions options_)
        : bus(bus_), options(std::move(options_)) {
        if (options.ping_interval.count() > 0) {
            watchdog = std::thread([this] { watch(); });
        }
    }
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
    ~State() {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (watchdog.joinable()) {
            watchdog.join();
        }
    }

    void add(const std::shared_ptr<HostProcess>& process) {
        const std::lock_guard lock(mutex);
        std::erase_if(processes, [](const auto& weak) { return weak.expired(); });
        processes.push_back(process);
    }

    void watch() {
        const auto tick = std::clamp(options.ping_interval / 4, std::chrono::milliseconds(10),
                                     std::chrono::milliseconds(1000));
        std::unique_lock lock(mutex);
        while (!stopping) {
            wake.wait_for(lock, tick, [this] { return stopping; });
            if (stopping) {
                return;
            }
            std::vector<std::shared_ptr<HostProcess>> alive;
            for (const auto& weak : processes) {
                if (auto process = weak.lock()) {
                    alive.push_back(std::move(process));
                }
            }
            lock.unlock();
            const Clock::time_point now = Clock::now();
            for (const auto& process : alive) {
                process->check(now, options.ping_interval, options.ping_timeout);
            }
            alive.clear();  // A process may end here, outside the lock.
            lock.lock();
        }
    }

    command::CommandBus& bus;
    const HostRuntimeOptions options;
    ConcurrentSignal<> pending;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::vector<std::weak_ptr<HostProcess>> processes;
    std::set<HostInstance*> instances;  // Used on the thread of the model only.
    std::thread watchdog;
};

namespace detail {

// --- HostInstance --------------------------------------------------------------------------------

/// An active out-of-process module: the process that runs it now and what it registered.
class HostInstance final : public ExtensionInstance {
public:
    HostInstance(const ExtensionInfo& extension, ExtensionHost& host,
                 std::shared_ptr<HostRuntime::State> state, HostLauncher launcher)
        : extension_(extension),
          host_(host),
          state_(std::move(state)),
          launcher_(std::move(launcher)),
          id_(extension.id()) {
        state_->instances.insert(this);
    }
    HostInstance(const HostInstance&) = delete;
    HostInstance& operator=(const HostInstance&) = delete;
    HostInstance(HostInstance&&) = delete;
    HostInstance& operator=(HostInstance&&) = delete;
    ~HostInstance() override {
        deactivate();
        state_->instances.erase(this);
    }

    /// Starts the process and activates the module in it. Throws ExtensionError(activation_failed).
    void start() {
        auto process =
            std::make_shared<HostProcess>(id_, [state = state_.get()] { state->pending.emit(); });
        try {
            process->start(launcher_(extension_));
        } catch (const std::exception& e) {
            throw ExtensionError(ExtensionErrc::activation_failed, e.what());
        }
        process_ = process;
        state_->add(process);
        Json site_packages = Json::array();
        if (extension_.manifest->kind == ExtensionKind::python) {
            site_packages.push_back(detail::display(extension_.folder / "site-packages"));
            if (!state_->options.user_site_packages.empty()) {
                site_packages.push_back(
                    detail::display(state_->options.user_site_packages /
                                    std::filesystem::path(std::u8string(id_.begin(), id_.end())) /
                                    "site-packages"));
            }
        }
        const Manifest& manifest = *extension_.manifest;
        const Json params = {{"protocolVersion", protocol_version},
                             {"product", {{"name", kProductName}, {"version", fem::version()}}},
                             {"extension",
                              {{"id", id_},
                               {"version", manifest.version.to_string()},
                               {"kind", to_string(*manifest.kind)},
                               {"entry", manifest.entry},
                               {"folder", detail::display(extension_.folder)}}},
                             {"sitePackages", std::move(site_packages)}};
        const Response response =
            process->wait(process->request("initialize", params), state_->options.startup_timeout,
                          [this](Message& m) { serve(m, false); });
        if (!response.ok()) {
            std::string reason = response.error->message;
            if (response.error->code == rpc_error::connection_lost) {
                process->wait_exit(state_->options.shutdown_timeout);
                reason = "it stopped while starting (" + process->stop_details() + ")";
            }
            shutdown_process();
            throw ExtensionError(ExtensionErrc::activation_failed, reason);
        }
        const Json& capabilities = response.result.value("capabilities", Json::object());
        shared_memory_ = capabilities.is_object() && capabilities.value("sharedMemory", false);
    }

    command::CommandResult execute(command::CommandContext& context) override {
        const std::string command = context.spec().id;
        ensure_running();
        if (!handlers_.contains(command)) {
            throw ExtensionError(ExtensionErrc::command_failed,
                                 "module '" + id_ + "' gave no code for command '" + command + "'");
        }
        const std::int64_t call = ++next_call_;
        calls_.emplace(call, &context);
        struct Forget {
            std::map<std::int64_t, command::CommandContext*>& calls;
            std::int64_t call;
            Forget(std::map<std::int64_t, command::CommandContext*>& c, std::int64_t id)
                : calls(c), call(id) {}
            Forget(const Forget&) = delete;
            Forget& operator=(const Forget&) = delete;
            ~Forget() { calls.erase(call); }
        } forget{calls_, call};

        const std::shared_ptr<HostProcess> process = process_;
        const Json params = {{"call", call},
                             {"command", command},
                             {"args", detail::to_json(command::Value(context.args().values()))}};
        const Response response =
            process->wait(process->request("command/execute", params), state_->options.call_timeout,
                          [this](Message& m) { serve(m, true); });
        if (!response.ok()) {
            if (response.error->code == rpc_error::connection_lost) {
                const CrashReason reason =
                    process->hung() ? CrashReason::hung : CrashReason::crashed;
                const bool restarted = stopped(reason, "while running '" + command + "'");
                throw ExtensionError(
                    ExtensionErrc::command_failed,
                    "module '" + id_ + "' " +
                        (reason == CrashReason::hung ? "stopped responding" : "crashed") +
                        " while running '" + command + "'" +
                        (restarted ? "; it was restarted" : "; it is not restarted any more"));
            }
            throw ExtensionError(ExtensionErrc::command_failed,
                                 "command '" + command + "' failed: " + response.error->message);
        }
        command::CommandResult result;
        const Json& body = response.result;
        if (body.is_object()) {
            result.value = detail::from_json(body.value("value", Json()));
            if (body.contains("task")) {
                const Json& task = body["task"];
                const std::string id = task.is_object() && task.contains("id")
                                           ? (task["id"].is_string() ? task["id"].get<std::string>()
                                                                     : task["id"].dump())
                                           : std::string();
                result.task = process->task(id);
                if (!result.task) {
                    // The process stopped right after answering: its tasks failed already.
                    task::TaskControl failed = task::make_task(id_);
                    failed.fail("module '" + id_ + "' stopped");
                    result.task = failed.task();
                }
            }
        }
        return result;
    }

    void deactivate() noexcept override {
        watches_.clear();
        model_changes_.reset();
        handlers_.clear();
        if (!process_) {
            return;
        }
        shutdown_process();
    }

    /// Does what the module asked outside its commands; restarts it if its process stopped.
    std::size_t poll() {
        if (!process_ || !calls_.empty()) {
            return 0;
        }
        std::size_t done = 0;
        if (process_->exited()) {
            const CrashReason reason = process_->hung() ? CrashReason::hung : CrashReason::crashed;
            stopped(reason, "while idle");
            return 1;
        }
        std::deque<Inbound> inbox = process_->take_inbox();
        for (Inbound& inbound : inbox) {
            serve(inbound.message, false);
            ++done;
        }
        while (!deferred_.empty() && process_ && !process_->exited()) {
            Message message = std::move(deferred_.front());
            deferred_.pop_front();
            run_deferred(message);
            ++done;
        }
        return done;
    }

    [[nodiscard]] bool running() const { return process_ && !process_->exited(); }

private:
    /// Restarts a process that stopped while nothing waited for it.
    void ensure_running() {
        if (broken_) {
            throw ExtensionError(ExtensionErrc::command_failed,
                                 "module '" + id_ + "' stopped " + std::to_string(stops_.size()) +
                                     " times and is not restarted; disable and enable it");
        }
        if (process_ && process_->exited()) {
            stopped(process_->hung() ? CrashReason::hung : CrashReason::crashed, "while idle");
        }
        if (broken_ || !process_) {
            throw ExtensionError(ExtensionErrc::command_failed,
                                 "module '" + id_ + "' is not running");
        }
    }

    /// The process stopped on its own or was ended as hung: forget what it registered, report,
    /// and start it again unless it stopped too often. Returns true if it was restarted.
    bool stopped(CrashReason reason, const std::string& when) {
        const std::shared_ptr<HostProcess> process = std::exchange(process_, nullptr);
        process->wait_exit(state_->options.shutdown_timeout);
        std::string details = process->stop_details() + " (" + when + ")";
        process->stop();
        watches_.clear();
        model_changes_.reset();
        handlers_.clear();
        deferred_.clear();

        const Clock::time_point now = Clock::now();
        std::erase_if(
            stops_, [&](Clock::time_point t) { return now - t > state_->options.restart_window; });
        stops_.push_back(now);
        const int count = static_cast<int>(stops_.size());
        bool restarted = false;
        if (count <= state_->options.restart_limit) {
            try {
                start();
                restarted = true;
            } catch (const std::exception& e) {
                details += "; restart failed: " + std::string(e.what());
            }
        }
        broken_ = !restarted;
        host_.report_crash(CrashReport{reason, restarted, count, details});
        return restarted;
    }

    /// Asks the process to stop, ends it if it does not, and waits for it.
    void shutdown_process() noexcept {
        const std::shared_ptr<HostProcess> process = std::exchange(process_, nullptr);
        if (!process) {
            return;
        }
        try {
            if (!process->exited()) {
                const Response response = process->wait(process->request("shutdown", nullptr),
                                                        state_->options.shutdown_timeout,
                                                        [this](Message& m) { serve(m, false); });
                (void)response;
                process->notify("exit");
                process->close_input();
                if (!process->wait_exit(state_->options.shutdown_timeout)) {
                    extension_log()->warn("module '{}' did not stop in time: ending it", id_);
                }
            }
        } catch (...) {
            // Ended below.
        }
        process->fail_tasks("module '" + id_ + "' was stopped");
        process->stop();
    }

    // --- Requests of the module ------------------------------------------------------------------

    void serve(Message& message, bool in_call) {
        const std::shared_ptr<HostProcess> process = process_;
        if (!process) {
            return;
        }
        if (message.method == "command/execute") {
            // A command of its own, outside of a call: not while another command runs.
            if (in_call || !calls_.empty()) {
                deferred_.push_back(std::move(message));
                state_->pending.emit();
            } else {
                run_deferred(message);
            }
            return;
        }
        try {
            process->respond(message, handle(message));
        } catch (const RequestError& e) {
            process->respond_error(message, e.code(), e.what());
        } catch (const std::exception& e) {
            process->respond_error(message, rpc_error::command_failed, e.what());
        }
    }

    void run_deferred(Message& message) {
        const std::shared_ptr<HostProcess> process = process_;
        if (!process) {
            return;
        }
        try {
            const std::string command = string_param(message.params, "command");
            const command::CommandResult result =
                state_->bus.execute(command, dict_param(message.params, "args"));
            process->respond(message, {{"value", detail::to_json(result.value)}});
        } catch (const RequestError& e) {
            process->respond_error(message, e.code(), e.what());
        } catch (const std::exception& e) {
            process->respond_error(message, rpc_error::command_failed, e.what());
        }
    }

    command::CommandContext& call_context(const Json& params) {
        const std::int64_t call = integer_param(params, "call");
        const auto it = calls_.find(call);
        if (it == calls_.end()) {
            throw RequestError(rpc_error::not_found,
                               "call " + std::to_string(call) + " is not running");
        }
        return *it->second;
    }

    const model::Object& object(const Json& params) {
        const model::ObjectId id = object_param(param(params, "id"), "id");
        const model::Object* found = state_->bus.document().find(id);
        if (found == nullptr) {
            throw RequestError(rpc_error::not_found, "no object " + std::to_string(id.value()));
        }
        return *found;
    }

    Json handle(const Message& message) {
        const std::string& method = message.method;
        const Json& params = message.params;
        if (method == "contributions/register") {
            return register_contributions(params);
        }
        if (method == "context/declare") {
            const std::string type = string_param(params, "type");
            const auto kind = parse_key_type(type);
            if (!kind) {
                bad_params("unknown type of a context key '" + type + "'");
            }
            const Json initial = params.value("default", Json());
            return host_.declare_context_key(string_param(params, "name"), *kind,
                                             detail::to_context_value(initial));
        }
        if (method == "context/set") {
            host_.set_context_key(string_param(params, "name"),
                                  detail::to_context_value(param(params, "value")));
            return nullptr;
        }
        if (method == "context/evaluate") {
            return host_.evaluate_context(
                context::WhenExpression::parse(string_param(params, "when")));
        }
        if (method == "context/watch") {
            context::WhenExpression expression =
                context::WhenExpression::parse(string_param(params, "when"));
            const bool value = host_.evaluate_context(expression);
            const std::int64_t watch = ++next_watch_;
            std::weak_ptr<HostProcess> weak = process_;
            watches_.emplace(
                watch, host_.watch_context(std::move(expression), [weak, watch](bool now) {
                    if (const auto process = weak.lock()) {
                        process->notify("context/changed", {{"watch", watch}, {"value", now}});
                    }
                }));
            return {{"watch", watch}, {"value", value}};
        }
        if (method == "context/unwatch") {
            watches_.erase(integer_param(params, "watch"));
            return nullptr;
        }
        if (method == "settings/get") {
            try {
                return {{"value", host_.setting(string_param(params, "key"))}};
            } catch (const ExtensionError& e) {
                if (e.code() == ExtensionErrc::unknown_setting) {
                    throw RequestError(rpc_error::not_found, e.what());
                }
                throw;
            }
        }
        if (method == "model/subscribe") {
            std::weak_ptr<HostProcess> weak = process_;
            model_changes_ =
                state_->bus.on_model_changed([weak](const command::ModelChange& change) {
                    if (const auto process = weak.lock()) {
                        process->notify("model/changed", summary_json(change));
                    }
                });
            return nullptr;
        }
        if (method == "model/unsubscribe") {
            model_changes_.reset();
            return nullptr;
        }
        if (method == "document/objects") {
            return list_objects(params);
        }
        if (method == "document/object") {
            return detail::object_to_json(state_->bus.document(), object(params));
        }
        if (method == "document/data") {
            const model::Object& found = object(params);
            if (!found.data()) {
                return nullptr;
            }
            std::optional<ipc::SharedMemory> segment;
            Json data = detail::data_to_json(
                *found.data(),
                shared_memory_ ? std::optional<std::size_t>(state_->options.shared_memory_threshold)
                               : std::nullopt,
                segment);
            if (segment) {
                process_->keep(std::move(*segment));
            }
            return data;
        }
        if (method == "call/execute") {
            command::CommandContext& context = call_context(params);
            const command::CommandResult result =
                context.execute(string_param(params, "command"), dict_param(params, "args"));
            return {{"value", detail::to_json(result.value)}};
        }
        if (method == "call/createObject") {
            return {{"id", create_object(call_context(params), params).value()}};
        }
        if (method == "call/setData") {
            command::CommandContext& context = call_context(params);
            context.editor().set_data(object_param(param(params, "object"), "object"),
                                      detail::data_from_json(param(params, "data")));
            return nullptr;
        }
        throw RequestError(rpc_error::method_not_found, "unknown method '" + method + "'");
    }

    Json register_contributions(const Json& params) {
        const Json& commands = param(params, "commands");
        if (!commands.is_array()) {
            bad_params("'commands' must be a list of command ids");
        }
        const std::vector<CommandContribution>& declared =
            extension_.manifest->contributes.commands;
        std::vector<std::string> ids;
        for (const Json& command : commands) {
            if (!command.is_string()) {
                bad_params("'commands' must be a list of command ids");
            }
            const std::string id = command.get<std::string>();
            const bool known =
                std::any_of(declared.begin(), declared.end(),
                            [&](const CommandContribution& c) { return c.id == id; });
            if (!known) {
                bad_params("command '" + id + "' is not declared in the manifest of '" + id_ + "'");
            }
            ids.push_back(id);
        }
        handlers_.insert(ids.begin(), ids.end());
        return nullptr;
    }

    Json list_objects(const Json& params) {
        const std::string type = optional_string(params, "type");
        const std::string owner = optional_string(params, "owner");
        std::vector<std::uint64_t> ids;
        for (const model::Object& object : state_->bus.document().objects()) {
            if ((type.empty() || object.type() == type) &&
                (owner.empty() || object.owner() == owner)) {
                ids.push_back(object.id().value());
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    model::ObjectId create_object(command::CommandContext& context, const Json& params) {
        model::NewObject init;
        init.type = string_param(params, "type");
        init.owner = string_param(params, "owner");
        init.name = optional_string(params, "name");
        if (params.contains("schemaVersion")) {
            const std::int64_t version = integer_param(params, "schemaVersion");
            if (version < 1 || version > std::numeric_limits<std::uint32_t>::max()) {
                bad_params("'schemaVersion' must be a positive integer");
            }
            init.schema_version = static_cast<std::uint32_t>(version);
        }
        if (params.contains("properties") && !params["properties"].is_null()) {
            if (!params["properties"].is_object()) {
                bad_params("'properties' must be an object");
            }
            init.properties = params["properties"];
        }
        if (params.contains("parent") && !params["parent"].is_null()) {
            init.parent = object_param(params["parent"], "parent");
        }
        if (params.contains("dependencies") && !params["dependencies"].is_null()) {
            if (!params["dependencies"].is_array()) {
                bad_params("'dependencies' must be a list of object ids");
            }
            for (const Json& target : params["dependencies"]) {
                init.dependencies.push_back(object_param(target, "dependencies"));
            }
        }
        if (params.contains("data") && !params["data"].is_null()) {
            init.data = detail::data_from_json(params["data"]);
        }
        return context.editor().create(std::move(init));
    }

    const ExtensionInfo& extension_;
    ExtensionHost& host_;
    const std::shared_ptr<HostRuntime::State> state_;
    const HostLauncher launcher_;
    const std::string id_;
    std::shared_ptr<HostProcess> process_;
    bool shared_memory_ = false;
    std::set<std::string, std::less<>> handlers_;
    std::map<std::int64_t, command::CommandContext*> calls_;
    std::int64_t next_call_ = 0;
    std::map<std::int64_t, Subscription> watches_;
    std::int64_t next_watch_ = 0;
    Subscription model_changes_;
    std::deque<Message> deferred_;
    std::vector<Clock::time_point> stops_;
    bool broken_ = false;
};

}  // namespace detail

// --- Launchers -----------------------------------------------------------------------------------

HostLauncher process_launcher() {
    return [](const ExtensionInfo& extension) {
        job::JobSpec spec;
        spec.title = extension.id();
        const std::string& entry = extension.manifest->entry;
        spec.program =
            extension.folder / std::filesystem::path(std::u8string(entry.begin(), entry.end()));
        spec.workdir = extension.folder;
        return spec;
    };
}

HostLauncher python_launcher(
    std::filesystem::path program, std::vector<std::string> args,
    std::map<std::string, std::optional<std::string>, std::less<>> environment) {
    return [program = std::move(program), args = std::move(args),
            environment = std::move(environment)](const ExtensionInfo& extension) {
        job::JobSpec spec;
        spec.title = extension.id();
        spec.program = program;
        spec.args = args;
        spec.environment = environment;
        // Text of Python is UTF-8 whatever the locale of the system is.
        spec.environment.insert_or_assign("PYTHONUTF8", "1");
        spec.environment.insert_or_assign("PYTHONIOENCODING", "utf-8");
        spec.workdir = extension.folder;
        return spec;
    };
}

// --- HostRuntime ---------------------------------------------------------------------------------

HostRuntime::HostRuntime(command::CommandBus& bus, HostRuntimeOptions options)
    : state_(std::make_shared<State>(bus, std::move(options))) {}

HostRuntime::~HostRuntime() = default;

std::unique_ptr<ExtensionInstance> HostRuntime::activate(const ExtensionInfo& extension,
                                                         ExtensionHost& host) {
    const ExtensionKind kind = *extension.manifest->kind;
    const HostLauncher& launcher =
        kind == ExtensionKind::python ? state_->options.python : state_->options.process;
    if (kind == ExtensionKind::native || !launcher) {
        throw ExtensionError(
            ExtensionErrc::activation_failed,
            "this runtime cannot start " + std::string(to_string(kind)) + " modules");
    }
    auto instance = std::make_unique<HostInstance>(extension, host, state_, launcher);
    instance->start();
    return instance;
}

std::size_t HostRuntime::poll() {
    std::size_t done = 0;
    // A copy: restarting a module may report to listeners that disable modules.
    const std::vector<HostInstance*> instances(state_->instances.begin(), state_->instances.end());
    for (HostInstance* instance : instances) {
        if (state_->instances.contains(instance)) {
            done += instance->poll();
        }
    }
    return done;
}

Subscription HostRuntime::on_pending(std::function<void()> listener) {
    return state_->pending.subscribe(std::move(listener));
}

std::size_t HostRuntime::running() const {
    std::size_t count = 0;
    for (const HostInstance* instance : state_->instances) {
        count += instance->running() ? 1 : 0;
    }
    return count;
}

}  // namespace fem::extension
