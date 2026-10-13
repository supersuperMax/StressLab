#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/extension/host_runtime.h"
#include "fem/core/signal.h"

#include <functional>
#include <memory>

namespace fem::app {

/// The loop of out-of-process modules on the thread of the model. What modules ask outside their
/// commands waits for `HostRuntime::poll`; when the runtime has something (`on_pending`), the pump
/// asks the loop of that thread to call it (`call_soon`): headless, Python runs it between two
/// instructions of the script or while it waits for a task (`Interpreter::call_soon`, ADR 0011);
/// the window queues it in the event loop of Qt (ADR 0012). `poll` runs only at a step of its own
/// (`CommandBus::is_idle`): not inside a command or a transaction, where a module's command would
/// become a part of another step. Postponed work is tried again when the current step completes.
///
/// The runtime, the bus and the loop must outlive the pump.
class ModulePump {
public:
    /// Runs work on the thread of the model soon; called from any thread.
    using CallSoon = std::function<void(std::function<void()> work)>;

    ModulePump(extension::HostRuntime& runtime, command::CommandBus& bus, CallSoon call_soon);
    ModulePump(const ModulePump&) = delete;
    ModulePump& operator=(const ModulePump&) = delete;
    ModulePump(ModulePump&&) = delete;
    ModulePump& operator=(ModulePump&&) = delete;
    ~ModulePump();

    /// Does everything the modules have asked so far; for the end of a script. Call on the thread
    /// of the model, outside commands.
    void drain();

    struct State;

private:
    std::shared_ptr<State> state_;
    Subscription pending_;
    Subscription steps_;
};

}  // namespace fem::app
