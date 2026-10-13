#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/extension/runtime.h"
#include "fem/core/job/job_spec.h"
#include "fem/core/signal.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fem::extension {

struct ExtensionInfo;

/// Describes the process that runs the code of a module: what to start (a `JobSpec`, docs/
/// architecture.md §6); the runtime starts it on this computer with a channel to its input.
using HostLauncher = std::function<job::JobSpec(const ExtensionInfo& extension)>;

/// For process modules: the executable `entry` in the module folder (on Windows also with
/// ".exe"), started in that folder.
[[nodiscard]] HostLauncher process_launcher();

/// For python modules: a Python that runs the extension host, the package `stresslab_host`.
/// `program` and `args` start it, e.g. `python -m stresslab_host` or, in the application,
/// `stresslab --python -m stresslab_host`; `environment` is set on top of the inherited one (the
/// host itself drops PYTHONPATH and the user site-packages from its path, except its own folder).
[[nodiscard]] HostLauncher python_launcher(
    std::filesystem::path program, std::vector<std::string> args,
    std::map<std::string, std::optional<std::string>, std::less<>> environment = {});

struct HostRuntimeOptions {
    /// What to start for a module of each kind; a kind without a launcher cannot be activated.
    HostLauncher python;
    HostLauncher process = process_launcher();
    /// A module that does not answer `initialize` in this time fails to activate.
    std::chrono::milliseconds startup_timeout{30000};
    /// A command whose module sends nothing for this long is taken as hung: the module is ended
    /// and restarted, the command fails. Long work belongs in a Task (CLAUDE.md, rule 7).
    std::chrono::milliseconds call_timeout{30000};
    /// How often an idle module is asked "$/ping"; zero turns the watchdog off.
    std::chrono::milliseconds ping_interval{5000};
    /// A module that does not answer a ping in this time is taken as hung.
    std::chrono::milliseconds ping_timeout{10000};
    /// How long a disabled module may take to stop before it is ended.
    std::chrono::milliseconds shutdown_timeout{3000};
    /// A module that stops more often than this within `restart_window` is not restarted.
    int restart_limit = 3;
    std::chrono::milliseconds restart_window{std::chrono::minutes(5)};
    /// The site-packages of python modules that the user installs: `<root>/<id>/site-packages`.
    /// Empty: none.
    std::filesystem::path user_site_packages;
    /// Arrays of at least this many bytes go to modules through shared memory, if they support
    /// it; smaller ones are written into the message.
    std::size_t shared_memory_threshold = 4096;
};

/// The runtime of python and process modules (docs/architecture.md §4.4): each active module runs
/// in a process of its own and talks to the application by JSON-RPC 2.0 over its standard input
/// and output (docs/extensions/protocol.md); large arrays go through shared memory.
///
/// A python module runs in an extension host (`stresslab_host`), one process per module, with
/// the module folder and its own site-packages first on its path. A process module is an
/// executable that speaks the protocol itself.
///
/// If the process of a module crashes or hangs, the command it was running fails (the Command
/// Bus reverts it, so the model is intact), its tasks fail, and the process is started and the
/// module activated again; the manager sends "extension.crashed". After `restart_limit` stops
/// within `restart_window` it is not restarted, and its commands fail until it is disabled and
/// enabled.
///
/// Threads: the model is single-threaded. What a module asks while one of its commands runs is
/// done on the thread of that command. What it asks at other times (from its background work)
/// waits for `poll`, which the application calls on the thread of the model; `on_pending` tells
/// when to. Progress of tasks is passed on at once, on the threads that read the processes.
class HostRuntime final : public ExtensionRuntime {
public:
    /// `bus` must outlive the runtime and the modules it activates.
    explicit HostRuntime(command::CommandBus& bus, HostRuntimeOptions options = {});
    HostRuntime(const HostRuntime&) = delete;
    HostRuntime& operator=(const HostRuntime&) = delete;
    HostRuntime(HostRuntime&&) = delete;
    HostRuntime& operator=(HostRuntime&&) = delete;
    ~HostRuntime() override;

    [[nodiscard]] std::unique_ptr<ExtensionInstance> activate(const ExtensionInfo& extension,
                                                              ExtensionHost& host) override;

    /// Does what the modules asked for outside of their commands and restarts modules whose
    /// process stopped. Call on the thread of the model. Returns how many things were done.
    std::size_t poll();
    /// Called, on any thread, when `poll` has something to do.
    [[nodiscard]] Subscription on_pending(std::function<void()> listener);

    /// How many module processes are running now (for tests and diagnostics).
    [[nodiscard]] std::size_t running() const;

    struct State;

private:
    std::shared_ptr<State> state_;
};

}  // namespace fem::extension
