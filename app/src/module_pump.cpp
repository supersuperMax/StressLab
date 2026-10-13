#include "module_pump.h"

#include <atomic>
#include <string_view>

namespace fem::app {

struct ModulePump::State : std::enable_shared_from_this<State> {
    State(extension::HostRuntime& runtime, command::CommandBus& bus, python::Interpreter& python)
        : runtime(runtime), bus(bus), python(python) {}

    /// Any thread: asks Python to pump soon, once however often the runtime calls.
    void request() {
        wanted = true;
        if (!scheduled.exchange(true)) {
            python.call_soon([weak = weak_from_this()] {
                if (const auto state = weak.lock()) {
                    state->pump();
                }
            });
        }
    }

    /// The thread of Python.
    void pump() {
        scheduled = false;
        if (polling || !bus.is_idle()) {
            return;  // `wanted` stays: the end of the step asks again.
        }
        wanted = false;
        polling = true;
        runtime.poll();
        polling = false;
        if (wanted) {
            request();
        }
    }

    void drain() {
        if (polling) {
            return;
        }
        polling = true;
        wanted = false;
        while (runtime.poll() > 0) {
        }
        polling = false;
    }

    extension::HostRuntime& runtime;
    command::CommandBus& bus;
    python::Interpreter& python;
    /// The runtime has work that has not been polled yet.
    std::atomic<bool> wanted{false};
    /// A call of `pump` is queued in Python.
    std::atomic<bool> scheduled{false};
    /// `poll` is running (it may run Python code, which may run the pump again).
    bool polling = false;
};

ModulePump::ModulePump(extension::HostRuntime& runtime, command::CommandBus& bus,
                       python::Interpreter& python)
    : state_(std::make_shared<State>(runtime, bus, python)) {
    pending_ = runtime.on_pending([state = std::weak_ptr<State>(state_)] {
        if (const auto locked = state.lock()) {
            locked->request();
        }
    });
    steps_ = bus.on_journal([this](std::string_view /*entry*/) {
        if (state_->wanted) {
            state_->request();
        }
    });
}

ModulePump::~ModulePump() = default;

void ModulePump::drain() {
    state_->drain();
}

}  // namespace fem::app
