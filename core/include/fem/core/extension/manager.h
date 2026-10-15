#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manifest.h"
#include "fem/core/extension/profile.h"
#include "fem/core/extension/registry.h"
#include "fem/core/extension/runtime.h"
#include "fem/core/extension/semver.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/document.h"
#include "fem/core/signal.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fem::extension {

/// Version of the platform that `engine` ranges of manifests are checked against: the version of
/// the core, `fem::version()`.
[[nodiscard]] Version engine_version();

/// Where a module was found.
enum class ExtensionSource : std::uint8_t {
    builtin,  ///< Shipped with the application.
    user,     ///< Installed by the user from the catalog.
    local,    ///< Installed from a folder; not verified, the shell warns before enabling it.
};

/// "builtin", "user", "local".
[[nodiscard]] std::string_view to_string(ExtensionSource source) noexcept;

enum class ExtensionState : std::uint8_t {
    disabled,  ///< Contributes nothing; its objects in the model are opaque.
    enabled,   ///< Its contributions are in the registry; its code is not loaded yet.
    active,    ///< Its code is loaded and activated.
};

/// "disabled", "enabled", "active".
[[nodiscard]] std::string_view to_string(ExtensionState state) noexcept;

/// An installed module, as the module manager shows it.
struct ExtensionInfo {
    std::shared_ptr<const Manifest> manifest;
    /// The folder of the module, absolute.
    std::filesystem::path folder;
    ExtensionSource source = ExtensionSource::local;
    /// Why this platform cannot run the module; empty if it can.
    std::string incompatibility;
    ExtensionState state = ExtensionState::disabled;
    /// Why the last activation failed; empty after a successful one and when disabled.
    std::string activation_error;
    /// The code of the module was loaded into the application process in this session and stays
    /// there until the next start, even if the module is disabled (native libraries).
    bool code_loaded = false;

    [[nodiscard]] const std::string& id() const noexcept { return manifest->id; }
    [[nodiscard]] bool compatible() const noexcept { return incompatibility.empty(); }
    [[nodiscard]] bool enabled() const noexcept { return state != ExtensionState::disabled; }
    [[nodiscard]] bool verified() const noexcept { return source != ExtensionSource::local; }
};

/// Sent by the Extension Manager to the shell and other listeners.
struct ExtensionEvent {
    enum class Kind : std::uint8_t {
        installed,  ///< It was added to the index (`install`, `scan`); it is disabled.
        /// It was removed from the index (`uninstall`), after it was disabled. Pointers to its
        /// `ExtensionInfo` and contributions are no longer valid.
        uninstalled,
        /// Its manifest was read again (`reload`); pointers to its contributions taken before are
        /// no longer valid. Sent while it is disabled, before it is enabled again.
        updated,
        enabled,            ///< Its contributions were added to the registry.
        activated,          ///< Its code was loaded and activated.
        activation_failed,  ///< Its code could not be activated; `message` says why.
        deactivated,        ///< It was disabled: everything it contributed is gone.
        /// Its code stopped (an out-of-process module crashed or hung); `crash` says what and
        /// whether it was restarted. The model is intact: the command it was running was undone.
        crashed,
    };

    Kind kind = Kind::enabled;
    std::string id;
    std::string message;
    /// For `crashed`.
    std::optional<CrashReport> crash;
};

/// "extension.installed", "extension.uninstalled", "extension.updated", "extension.enabled",
/// "extension.activated", "extension.activationFailed", "extension.deactivated",
/// "extension.crashed".
[[nodiscard]] std::string_view to_string(ExtensionEvent::Kind kind) noexcept;

/// The message the user sees when the code of a module stopped, in the language of
/// `localization` (strings "extension.crashed", "extension.hung", "extension.gaveUp" of the core).
[[nodiscard]] std::string crash_message(const ExtensionInfo& extension, const CrashReport& crash,
                                        const l10n::Localization& localization);

/// A command of an installed module, enabled or not, for the command palette: a command of a
/// disabled module is offered with "enable the module" (docs/architecture.md §4.6, item 8).
struct IndexedCommand {
    const CommandContribution* command = nullptr;
    const ExtensionInfo* extension = nullptr;

    [[nodiscard]] bool enabled() const noexcept { return extension->enabled(); }
};

/// A folder that `scan` could not install.
struct ScanProblem {
    std::filesystem::path folder;
    std::string message;
    /// Problems of the manifest with their lines, if that was the reason.
    std::vector<ManifestDiagnostic> diagnostics;
};

struct ScanReport {
    std::vector<std::string> installed;  ///< Ids of the modules installed by this scan.
    std::vector<ScanProblem> problems;
};

/// A module of a profile that did not end up as the profile wants.
struct ProfileIssue {
    std::string id;
    std::string message;
};

/// What `ExtensionManager::apply_profile` would do now, by id.
struct ProfilePlan {
    /// Modules it would enable: those of the profile and what they require.
    std::vector<std::string> enable;
    /// Enabled modules it would disable: neither in the profile nor required by it.
    std::vector<std::string> disable;
};

/// What `ExtensionManager::apply_profile` did.
struct ProfileReport {
    std::vector<std::string> enabled;   ///< Enabled by the profile, with dependencies.
    std::vector<std::string> disabled;  ///< Disabled because the profile does not list them.
    std::vector<std::string> missing;   ///< Listed in the profile but not installed.
    /// Installed in another version than the profile lists; enabled anyway.
    std::vector<ProfileIssue> version_mismatches;
    /// Installed but could not be enabled (incompatible, missing dependency, conflict).
    std::vector<ProfileIssue> failed;
};

/// What `ExtensionManager::reload` did with the modules it had to disable.
struct ReloadReport {
    /// Enabled again with the new manifest, the reloaded module first.
    std::vector<std::string> enabled;
    /// Could not be enabled again (the new manifest is incompatible, needs a missing module, takes
    /// an id of another module); they stay disabled.
    std::vector<ProfileIssue> failed;
};

/// A module installed from a folder whose manifest there has another version now: an update the
/// module manager offers ("Updates").
struct ExtensionUpdate {
    std::string id;
    Version installed;
    Version available;
};

/// A module the objects of the document belong to, which is not enabled: its objects are opaque
/// (docs/architecture.md §4.6 item 4; the bar "The model needs the modules…" of
/// docs/interface-design.md §6, "Profiles").
struct NeededModule {
    std::string id;
    /// Objects of the document it owns.
    std::size_t objects = 0;
    /// The installed module; nullptr if it is not installed.
    const ExtensionInfo* extension = nullptr;

    /// It is installed and this platform can run it.
    [[nodiscard]] bool can_enable() const noexcept {
        return extension != nullptr && extension->compatible();
    }
};

/// Whether enabling and disabling modules is written to the journal of the Command Bus. The user
/// enabling a module (the module manager, `stresslab.extensions.enable`) is journaled, so that the
/// journal replays with the same modules; the application enabling modules at startup is not.
enum class Journaling : std::uint8_t {
    off,
    /// Each call is written as `stresslab.extensions.enable("id")` or `disable("id")`, also when
    /// the module already was in that state. Refused inside a command or a transaction, whose undo
    /// could not take it back.
    on,
};

/// Reads the value in effect of a setting for the code of a module (`ExtensionHost::setting`);
/// throws ExtensionError(unknown_setting) for a key without a schema. `bind_settings` gives the
/// manager one.
using SettingReader = std::function<nlohmann::json(std::string_view key)>;

struct ExtensionManagerOptions {
    /// Version of the platform for `engine` ranges; tests set their own.
    Version engine = engine_version();
};

/// Finds, enables, activates and disables modules (docs/architecture.md §4.1–4.6).
///
/// A module is *installed* when its manifest is in the index. *Enabling* it is declarative: its
/// contributions go to the registry, its commands are registered on the Command Bus (with the
/// argument schema of the manifest), the context keys of its manifest are declared and its objects
/// in the model stop being opaque. Its code is loaded only on *activation*: when an event of its
/// `activation` list fires (`activate_on`, `startup`) or when one of its commands runs. Required
/// modules are enabled before and activated before the modules that need them.
///
/// *Disabling* a module works without a restart: its code is deactivated, its commands are
/// unregistered, its context keys and registry records removed, its objects become opaque, and
/// "extension.deactivated" is sent. A native library stays loaded until the next start;
/// enabling the module again in the same session activates the library already loaded.
///
/// The manager uses a document, its Command Bus and the context keys, which must outlive it.
/// Single-threaded, like the model.
class ExtensionManager {
public:
    /// Throws ExtensionError(invalid_state) if `document` is not the document of `bus`.
    ExtensionManager(model::Document& document, command::CommandBus& bus,
                     context::ContextKeys& keys, ExtensionManagerOptions options = {});
    ExtensionManager(const ExtensionManager&) = delete;
    ExtensionManager& operator=(const ExtensionManager&) = delete;
    ExtensionManager(ExtensionManager&&) = delete;
    ExtensionManager& operator=(ExtensionManager&&) = delete;
    /// Deactivates the code of the modules and removes their commands and context keys, without
    /// events.
    ~ExtensionManager();

    [[nodiscard]] const Version& engine() const noexcept { return options_.engine; }
    /// Contributions of the enabled modules.
    [[nodiscard]] const ContributionRegistry& registry() const noexcept { return registry_; }

    // --- Index -----------------------------------------------------------------------------------

    /// Installs every module in the subfolders of `root` (one module per folder, with
    /// `stresslab-extension.toml`), in the order of the folder names. Folders without a manifest
    /// are skipped; bad manifests and ids that are already installed from another folder are
    /// reported, not thrown. A folder installed before is skipped quietly.
    ScanReport scan(const std::filesystem::path& root, ExtensionSource source);
    /// Installs the module in `folder`. Returns the module already installed from this folder, if
    /// any. Throws ExtensionError(io) or ManifestError for its manifest and
    /// ExtensionError(conflict) if the id is installed from another folder.
    const ExtensionInfo& install(const std::filesystem::path& folder, ExtensionSource source);
    /// Installs a module from a parsed manifest; its files are in `folder`. Throws as above.
    /// Each module installed sends "extension.installed".
    const ExtensionInfo& install(Manifest manifest, const std::filesystem::path& folder,
                                 ExtensionSource source);
    /// Removes a module from the index: disables it and the modules that need it first, as
    /// `disable` does, then forgets it and sends "extension.uninstalled". Its folder stays on disk
    /// and its objects in the model stay opaque; a native library stays loaded until the next
    /// start. Modules that need it stay installed and cannot be enabled until it is installed
    /// again. Throws ExtensionError(unknown_extension), ExtensionError(invalid_state) for a module
    /// shipped with the application, and as `disable`.
    void uninstall(std::string_view id);
    /// Reads the manifest of a module from its folder again: the module was changed there (an
    /// update of a local module). If the module was enabled, it and the enabled modules that need
    /// it are disabled, the new manifest replaces the old one ("extension.updated"), and they are
    /// enabled again; what cannot be enabled any more is reported and stays disabled. A native
    /// library loaded in this session is not reloaded until the next start. The `ExtensionInfo`
    /// of the module stays where it was. Throws ExtensionError(unknown_extension),
    /// ExtensionError(io) and ManifestError for the manifest, ExtensionError(conflict) if the
    /// folder has a module of another id now, and as `disable`; then nothing changes.
    ReloadReport reload(std::string_view id);
    /// Modules not shipped with the application whose folder has a manifest of another version
    /// now, sorted by id. Manifests that cannot be read are skipped.
    [[nodiscard]] std::vector<ExtensionUpdate> updates() const;

    /// Installed modules, sorted by id.
    [[nodiscard]] std::vector<const ExtensionInfo*> extensions() const;
    /// The module, or nullptr.
    [[nodiscard]] const ExtensionInfo* find(std::string_view id) const;
    /// The module; throws ExtensionError(unknown_extension).
    [[nodiscard]] const ExtensionInfo& get(std::string_view id) const;
    /// Commands of all installed modules, enabled or not, sorted by command id, then module id.
    [[nodiscard]] std::vector<IndexedCommand> command_index() const;

    // --- Enabling --------------------------------------------------------------------------------

    /// Modules `enable(id)` would enable, required modules first; empty if `id` is enabled. Throws
    /// ExtensionError(unknown_extension, incompatible, unresolved_dependency, dependency_cycle).
    [[nodiscard]] std::vector<std::string> plan_enable(std::string_view id) const;
    /// Enables the module and the modules it requires, and returns them as `plan_enable` does.
    /// Throws as `plan_enable`, and ExtensionError(conflict) if an id they contribute is taken;
    /// then nothing is enabled. After `startup`, modules waiting for "onStartup" are activated at
    /// once. With `Journaling::on` the call is journaled after it succeeds; inside a command or a
    /// transaction it throws ExtensionError(invalid_state) and enables nothing.
    std::vector<std::string> enable(std::string_view id, Journaling journaling = Journaling::off);
    /// Modules `disable(id)` would disable: the enabled modules that need it, then itself; empty if
    /// it is disabled. Throws ExtensionError(unknown_extension).
    [[nodiscard]] std::vector<std::string> plan_disable(std::string_view id) const;
    /// Disables the module and the modules that need it, and returns them as `plan_disable` does;
    /// sends "extension.deactivated" for each. Throws ExtensionError(invalid_state) and disables
    /// nothing while a command of one of them is running. `journaling` as for `enable`.
    std::vector<std::string> disable(std::string_view id, Journaling journaling = Journaling::off);

    /// Modules that own objects of the document but are not enabled, sorted by id: the modules the
    /// model needs. Objects of the core never count.
    [[nodiscard]] std::vector<NeededModule> needed_modules() const;

    // --- Activation ------------------------------------------------------------------------------

    /// The runtime for modules of `kind`, replacing the one there; nullptr removes it. The core
    /// has the native runtime; python and process modules need runtimes from task 1.9.
    void set_runtime(ExtensionKind kind, std::shared_ptr<ExtensionRuntime> runtime);
    /// Marks the application as started and activates the modules waiting for "onStartup".
    void startup();
    [[nodiscard]] bool started() const noexcept { return started_; }
    /// Activates the enabled modules whose activation events match `event`, and the modules they
    /// require; returns the ids activated. A failure is sent as "extension.activationFailed", not
    /// thrown, and a module whose activation failed is not tried again by events until it is
    /// disabled and enabled or activated explicitly.
    std::vector<std::string> activate_on(const ActivationEvent& event);
    /// Activates an enabled module and the modules it requires. Does nothing for an active module
    /// or a module without code. Throws ExtensionError(invalid_state) for a disabled module and
    /// ExtensionError(activation_failed).
    void activate(std::string_view id);

    // --- Profiles --------------------------------------------------------------------------------

    /// The enabled modules with their versions, as a profile.
    [[nodiscard]] Profile profile(std::string name,
                                  nlohmann::json layout = nlohmann::json::object()) const;
    /// What `apply_profile` would enable and disable, sorted by id; modules of the profile that
    /// cannot be enabled are left out.
    [[nodiscard]] ProfilePlan plan_profile(const Profile& profile) const;
    /// Enables the modules of the profile (with their dependencies) and disables the others. With
    /// `Journaling::on` it journals `disable` for each module of `ProfileReport::disabled`, then
    /// `enable` for each of `ProfileReport::enabled`; refused as `enable` refuses.
    ProfileReport apply_profile(const Profile& profile, Journaling journaling = Journaling::off);

    // --- Settings --------------------------------------------------------------------------------

    /// Where the code of modules reads settings from; an empty reader: nowhere.
    void set_setting_reader(SettingReader reader) { setting_reader_ = std::move(reader); }

    // --- Events ----------------------------------------------------------------------------------

    [[nodiscard]] Subscription on_event(std::function<void(const ExtensionEvent&)> listener);

private:
    struct Record;

    [[nodiscard]] Record* find_record(std::string_view id) const;
    [[nodiscard]] Record& get_record(std::string_view id) const;
    /// `id` and the modules it requires, required first. With `skip_enabled`, enabled modules
    /// (and what they require) are left out.
    [[nodiscard]] std::vector<std::string> resolve(std::string_view id, bool skip_enabled) const;
    void check_conflicts(const std::vector<std::string>& plan) const;
    /// Gives a disabled record its manifest and checks whether this platform can run it.
    void set_manifest(Record& record, Manifest manifest) const;
    void enable_one(Record& record);
    void disable_one(Record& record);
    /// Activates `record` and what it requires; appends what was activated. Throws
    /// ExtensionError(activation_failed).
    void activate_record(Record& record, std::vector<std::string>& activated);
    command::CommandResult run_command(const std::string& owner, command::CommandContext& context);
    void emit(ExtensionEvent::Kind kind, const std::string& id, std::string message = {},
              std::optional<CrashReport> crash = std::nullopt);
    /// Throws ExtensionError(invalid_state) if `journaling` is on but the bus is not idle.
    void check_journaling(Journaling journaling) const;
    /// Writes `stresslab.extensions.<action>("id")` to the journal.
    void journal(std::string_view action, std::string_view id);
    friend class ExtensionHost;

    model::Document& document_;
    command::CommandBus& bus_;
    context::ContextKeys& keys_;
    ExtensionManagerOptions options_;
    ContributionRegistry registry_;
    std::map<std::string, std::unique_ptr<Record>, std::less<>> records_;
    std::map<ExtensionKind, std::shared_ptr<ExtensionRuntime>> runtimes_;
    bool started_ = false;
    SettingReader setting_reader_;
    Signal<const ExtensionEvent&> events_;
};

}  // namespace fem::extension
