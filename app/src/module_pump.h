#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/extension/host_runtime.h"
#include "fem/core/signal.h"
#include "fem/python/interpreter.h"

#include <memory>

namespace fem::app {

/// The loop of out-of-process modules while Python runs a script on the thread of the model
/// (headless, ADR 0011). What modules ask outside their commands waits for `HostRuntime::poll`;
/// when the runtime has something (`on_pending`), the pump asks Python to call it on its thread
/// (`Interpreter::call_soon`): between two instructions of the script or while it waits for a
/// task. `poll` runs only at a step of its own (`CommandBus::is_idle`): not inside a command or a
/// transaction of the script, where a module's command would become a part of the script's step.
/// Postponed work is tried again when the current step completes.
///
/// The runtime, the bus and the interpreter must outlive the pump.
class ModulePump {
public:
    ModulePump(extension::HostRuntime& runtime, command::CommandBus& bus,
               python::Interpreter& python);
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
