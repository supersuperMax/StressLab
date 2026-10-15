#pragma once

#include "fem/core/command/command.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/value.h"
#include "fem/core/context/when.h"
#include "fem/core/signal.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace fem::extension {

class ExtensionManager;
struct ExtensionInfo;

/// Why the code of a running module stopped (out-of-process modules).
enum class CrashReason : std::uint8_t {
    crashed,  ///< Its process ended unexpectedly.
    hung,     ///< It stopped answering and was ended.
};

/// "crashed", "hung".
[[nodiscard]] std::string_view to_string(CrashReason reason) noexcept;

/// What a runtime tells the application when the code of a module stopped.
struct CrashReport {
    CrashReason reason = CrashReason::crashed;
    /// The code was started and activated again; otherwise it crashed too often and its commands
    /// fail until the module is disabled and enabled.
    bool restarted = false;
    /// How many times it stopped within the restart window, this time included.
    int count = 1;
    /// For the log: exit code, the last lines it wrote to its error output, the command it ran.
    std::string details;
};

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
    /// The value of a `when` expression over the context keys.
    [[nodiscard]] bool evaluate_context(const context::WhenExpression& expression) const;
    /// Calls `listener` with the new value whenever the value of `expression` changes, as
    /// `ContextKeys::watch` does. The subscription must not outlive the module's activation.
    [[nodiscard]] Subscription watch_context(context::WhenExpression expression,
                                             std::function<void(bool)> listener);
    /// The value in effect of a setting: one of the module's own (`contributes.settings`) or of
    /// another part of the application. Throws ExtensionError(unknown_setting) for a key without
    /// a schema, or when the application gives modules no settings.
    [[nodiscard]] nlohmann::json setting(std::string_view key) const;
    /// Tells the application that the code of the module stopped: the manager logs it and sends
    /// "extension.crashed" with the report, so the shell can show a message.
    void report_crash(CrashReport report);

private:
    friend class ExtensionManager;

    ExtensionHost(const ExtensionInfo& extension, context::ContextKeys& keys,
                  ExtensionManager& manager) noexcept
        : extension_(extension), keys_(keys), manager_(manager) {}

    const ExtensionInfo& extension_;
    context::ContextKeys& keys_;
    ExtensionManager& manager_;
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
/// (`HostRuntime`, task 1.9) and are given to `ExtensionManager::set_runtime`.
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
