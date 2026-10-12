#pragma once

#include "fem/core/command/command.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/value.h"

#include <memory>
#include <string>
#include <string_view>

namespace fem::extension {

struct ExtensionInfo;

/// What the code of an active module may do in the application besides running its commands.
/// Given by the Extension Manager to `ExtensionRuntime::activate`; valid until the module is
/// deactivated.
class ExtensionHost {
public:
    ExtensionHost(const ExtensionHost&) = delete;
    ExtensionHost& operator=(const ExtensionHost&) = delete;
    ExtensionHost(ExtensionHost&&) = delete;
    ExtensionHost& operator=(ExtensionHost&&) = delete;
    ~ExtensionHost() = default;

    [[nodiscard]] const ExtensionInfo& extension() const noexcept { return extension_; }

    /// Declares a context key owned by the module; it is removed when the module is disabled.
    /// Returns and throws as `ContextKeys::declare`.
    bool declare_context_key(std::string name, context::ContextValue::Kind type,
                             context::ContextValue initial = {});
    /// Sets a context key of the module, declared in its manifest (`contextKeys`) or by
    /// `declare_context_key`. Throws ExtensionError(conflict) for a key of another owner and
    /// ContextError as `ContextKeys::set` does.
    void set_context_key(std::string_view name, context::ContextValue value);

private:
    friend class ExtensionManager;

    ExtensionHost(const ExtensionInfo& extension, context::ContextKeys& keys) noexcept
        : extension_(extension), keys_(keys) {}

    const ExtensionInfo& extension_;
    context::ContextKeys& keys_;
};

/// The loaded and activated code of a module.
class ExtensionInstance {
public:
    ExtensionInstance() = default;
    ExtensionInstance(const ExtensionInstance&) = delete;
    ExtensionInstance& operator=(const ExtensionInstance&) = delete;
    ExtensionInstance(ExtensionInstance&&) = delete;
    ExtensionInstance& operator=(ExtensionInstance&&) = delete;
    virtual ~ExtensionInstance() = default;

    /// Runs a command the module contributes, `context.spec().id`, as its handler on the Command
    /// Bus: changes go through `context.editor()` or commands run by `context.execute`. Throws
    /// ExtensionError(command_failed) when the code reports an error; the bus then reverts the
    /// changes.
    virtual command::CommandResult execute(command::CommandContext& context) = 0;
    /// Called once, when the module is disabled or the manager is destroyed. Code that cannot be
    /// unloaded from a running process (native libraries) stays loaded until the next start.
    virtual void deactivate() noexcept = 0;
};

/// Loads and activates the code of one kind of module (`ExtensionKind`). The core has the runtime
/// of native modules; the runtimes of python and process modules come with the extension host
/// (task 1.9) and are given to `ExtensionManager::set_runtime`.
class ExtensionRuntime {
public:
    ExtensionRuntime() = default;
    ExtensionRuntime(const ExtensionRuntime&) = delete;
    ExtensionRuntime& operator=(const ExtensionRuntime&) = delete;
    ExtensionRuntime(ExtensionRuntime&&) = delete;
    ExtensionRuntime& operator=(ExtensionRuntime&&) = delete;
    virtual ~ExtensionRuntime() = default;

    /// Loads the code of an enabled module and activates it. `host` outlives the instance. Throws
    /// on failure, preferably ExtensionError(activation_failed) with the reason.
    [[nodiscard]] virtual std::unique_ptr<ExtensionInstance> activate(
        const ExtensionInfo& extension, ExtensionHost& host) = 0;
    /// True if code this runtime loaded stays in memory until the next start (native libraries).
    [[nodiscard]] virtual bool keeps_code_loaded() const noexcept { return false; }
};

}  // namespace fem::extension
