#include "fem/core/extension/manager.h"

#include "fem/core/command/python_literal.h"
#include "fem/core/extension/error.h"
#include "fem/core/log/log.h"
#include "fem/core/product.h"
#include "fem/core/version.h"

#include "files.h"
#include "native_runtime.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <set>
#include <utility>

namespace fem::extension {

namespace {

std::string join(const std::vector<std::string>& ids, std::string_view separator) {
    std::string text;
    for (const std::string& id : ids) {
        if (!text.empty()) {
            text += separator;
        }
        text += id;
    }
    return text;
}

std::filesystem::path normalized(const std::filesystem::path& folder) {
    return std::filesystem::absolute(folder).lexically_normal();
}

}  // namespace

// --- Free functions ------------------------------------------------------------------------------

Version engine_version() {
    return Version::parse(fem::version());
}

std::string_view to_string(ExtensionSource source) noexcept {
    switch (source) {
        case ExtensionSource::builtin:
            return "builtin";
        case ExtensionSource::user:
            return "user";
        case ExtensionSource::local:
            return "local";
    }
    return "unknown";
}

std::string_view to_string(ExtensionState state) noexcept {
    switch (state) {
        case ExtensionState::disabled:
            return "disabled";
        case ExtensionState::enabled:
            return "enabled";
        case ExtensionState::active:
            return "active";
    }
    return "unknown";
}

std::string_view to_string(ExtensionEvent::Kind kind) noexcept {
    switch (kind) {
        case ExtensionEvent::Kind::installed:
            return "extension.installed";
        case ExtensionEvent::Kind::uninstalled:
            return "extension.uninstalled";
        case ExtensionEvent::Kind::updated:
            return "extension.updated";
        case ExtensionEvent::Kind::enabled:
            return "extension.enabled";
        case ExtensionEvent::Kind::activated:
            return "extension.activated";
        case ExtensionEvent::Kind::activation_failed:
            return "extension.activationFailed";
        case ExtensionEvent::Kind::deactivated:
            return "extension.deactivated";
        case ExtensionEvent::Kind::crashed:
            return "extension.crashed";
    }
    return "unknown";
}

std::string_view to_string(CrashReason reason) noexcept {
    switch (reason) {
        case CrashReason::crashed:
            return "crashed";
        case CrashReason::hung:
            return "hung";
    }
    return "unknown";
}

std::string crash_message(const ExtensionInfo& extension, const CrashReport& crash,
                          const l10n::Localization& localization) {
    const std::string name = localization.manifest_text(extension.id(), extension.manifest->name);
    const l10n::Args args{{"module", name}, {"count", std::int64_t{crash.count}}};
    if (!crash.restarted) {
        return localization.text(model::core_owner, "extension.gaveUp", args);
    }
    return localization.text(
        model::core_owner,
        crash.reason == CrashReason::hung ? "extension.hung" : "extension.crashed", args);
}

// --- ExtensionHost -------------------------------------------------------------------------------

bool ExtensionHost::declare_context_key(std::string name, context::ContextValue::Kind type,
                                        context::ContextValue initial) {
    return keys_.declare({.name = std::move(name),
                          .owner = extension_.id(),
                          .type = type,
                          .initial = std::move(initial)});
}

void ExtensionHost::set_context_key(std::string_view name, context::ContextValue value) {
    if (const context::KeySpec* spec = keys_.find(name);
        spec != nullptr && spec->owner != extension_.id()) {
        throw ExtensionError(ExtensionErrc::conflict, "context key '" + std::string(name) +
                                                          "' belongs to '" + spec->owner +
                                                          "', not to '" + extension_.id() + "'");
    }
    keys_.set(name, std::move(value));
}

bool ExtensionHost::evaluate_context(const context::WhenExpression& expression) const {
    return keys_.evaluate(expression);
}

Subscription ExtensionHost::watch_context(context::WhenExpression expression,
                                          std::function<void(bool)> listener) {
    return keys_.watch(std::move(expression), std::move(listener));
}

nlohmann::json ExtensionHost::setting(std::string_view key) const {
    if (!manager_.setting_reader_) {
        throw ExtensionError(
            ExtensionErrc::unknown_setting,
            "the application gives modules no settings ('" + std::string(key) + "' was asked)");
    }
    return manager_.setting_reader_(key);
}

void ExtensionHost::report_crash(CrashReport report) {
    const std::string& id = extension_.id();
    log::logger("extension")
        ->error("module '{}' {} ({} time(s) in a row); {}: {}", id, to_string(report.reason),
                report.count, report.restarted ? "restarted" : "not restarted any more",
                report.details);
    std::string message = report.details;
    manager_.emit(ExtensionEvent::Kind::crashed, id, std::move(message), std::move(report));
}

// --- ExtensionManager ----------------------------------------------------------------------------

struct ExtensionManager::Record {
    ExtensionInfo info;
    /// The contributions of the manifest, sharing its lifetime.
    std::shared_ptr<const Contributions> contributions;
    std::unique_ptr<ExtensionHost> host;
    std::unique_ptr<ExtensionInstance> instance;
    int running = 0;  // Commands of the module being run.
};

ExtensionManager::ExtensionManager(model::Document& document, command::CommandBus& bus,
                                   context::ContextKeys& keys, ExtensionManagerOptions options)
    : document_(document), bus_(bus), keys_(keys), options_(std::move(options)) {
    if (&bus.document() != &document) {
        throw ExtensionError(ExtensionErrc::invalid_state,
                             "the Command Bus edits another document");
    }
    runtimes_[ExtensionKind::native] = std::make_shared<detail::NativeRuntime>(options_.engine);
}

ExtensionManager::~ExtensionManager() {
    for (auto& [id, record] : records_) {
        if (!record->info.enabled()) {
            continue;
        }
        if (record->instance) {
            record->instance->deactivate();
            record->instance.reset();
        }
        record->host.reset();
        try {
            bus_.unregister_owner(id);
            keys_.remove_owner(id);
            document_.set_owner_enabled(id, false);
        } catch (...) {
            // Nothing to report to from a destructor; the ids are valid, so this does not happen.
        }
    }
}

// --- Index ---------------------------------------------------------------------------------------

ScanReport ExtensionManager::scan(const std::filesystem::path& root, ExtensionSource source) {
    ScanReport report;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        report.problems.push_back({root, "'" + detail::display(root) + "' is not a folder", {}});
        return report;
    }
    std::vector<std::filesystem::path> folders;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end;
         it.increment(error)) {
        std::error_code ignored;
        if (it->is_directory(ignored)) {
            folders.push_back(it->path());
        }
    }
    if (error) {
        report.problems.push_back(
            {root, "cannot list '" + detail::display(root) + "': " + error.message(), {}});
    }
    std::sort(folders.begin(), folders.end());
    for (const std::filesystem::path& folder : folders) {
        std::error_code ignored;
        if (!std::filesystem::is_regular_file(folder / manifest_file_name, ignored)) {
            continue;
        }
        const std::filesystem::path absolute = normalized(folder);
        const bool known = std::any_of(records_.begin(), records_.end(), [&](const auto& entry) {
            return entry.second->info.folder == absolute;
        });
        if (known) {
            continue;
        }
        try {
            report.installed.push_back(install(folder, source).id());
        } catch (const ManifestError& e) {
            report.problems.push_back({folder, e.what(), e.diagnostics()});
        } catch (const ExtensionError& e) {
            report.problems.push_back({folder, e.what(), {}});
        }
    }
    return report;
}

const ExtensionInfo& ExtensionManager::install(const std::filesystem::path& folder,
                                               ExtensionSource source) {
    const std::filesystem::path absolute = normalized(folder);
    for (const auto& [id, record] : records_) {
        if (record->info.folder == absolute) {
            return record->info;
        }
    }
    return install(load_manifest(absolute / manifest_file_name, options_.engine), absolute, source);
}

const ExtensionInfo& ExtensionManager::install(Manifest manifest,
                                               const std::filesystem::path& folder,
                                               ExtensionSource source) {
    const std::filesystem::path absolute = normalized(folder);
    if (const Record* existing = find_record(manifest.id)) {
        if (existing->info.folder == absolute) {
            return existing->info;
        }
        throw ExtensionError(ExtensionErrc::conflict,
                             "module '" + manifest.id + "' is already installed from '" +
                                 detail::display(existing->info.folder) + "'");
    }
    auto record = std::make_unique<Record>();
    set_manifest(*record, std::move(manifest));
    record->info.folder = absolute;
    record->info.source = source;
    const std::string id = record->info.id();
    const ExtensionInfo& info = records_.emplace(id, std::move(record)).first->second->info;
    emit(ExtensionEvent::Kind::installed, id);
    return info;
}

void ExtensionManager::set_manifest(Record& record, Manifest manifest) const {
    record.info.manifest = std::make_shared<const Manifest>(std::move(manifest));
    record.contributions = std::shared_ptr<const Contributions>(record.info.manifest,
                                                                &record.info.manifest->contributes);
    const Manifest& installed = *record.info.manifest;
    record.info.incompatibility.clear();
    if (!installed.engine.contains(options_.engine)) {
        record.info.incompatibility = "it needs " + std::string(kProductName) + " " +
                                      installed.engine.to_string() + ", this is " +
                                      options_.engine.to_string();
    }
}

void ExtensionManager::uninstall(std::string_view id) {
    const Record& record = get_record(id);
    if (record.info.source == ExtensionSource::builtin) {
        throw ExtensionError(ExtensionErrc::invalid_state,
                             "module '" + std::string(id) +
                                 "' is shipped with the application and cannot be uninstalled");
    }
    (void)disable(id);
    const std::string removed(id);
    records_.erase(records_.find(id));
    emit(ExtensionEvent::Kind::uninstalled, removed);
}

ReloadReport ExtensionManager::reload(std::string_view id) {
    Record& record = get_record(id);
    Manifest manifest = load_manifest(record.info.folder / manifest_file_name, options_.engine);
    if (manifest.id != record.info.id()) {
        throw ExtensionError(ExtensionErrc::conflict, "the folder '" +
                                                          detail::display(record.info.folder) +
                                                          "' of module '" + record.info.id() +
                                                          "' has module '" + manifest.id + "' now");
    }
    // Disabled in this order (the modules that need it first), enabled again the other way.
    std::vector<std::string> disabled = disable(id);
    set_manifest(record, std::move(manifest));
    emit(ExtensionEvent::Kind::updated, record.info.id());
    ReloadReport report;
    for (auto it = disabled.rbegin(); it != disabled.rend(); ++it) {
        const Record* module = find_record(*it);
        if (module == nullptr || module->info.enabled()) {
            continue;
        }
        try {
            for (std::string& enabled : enable(*it)) {
                report.enabled.push_back(std::move(enabled));
            }
        } catch (const ExtensionError& e) {
            report.failed.push_back({*it, e.what()});
        }
    }
    return report;
}

std::vector<ExtensionUpdate> ExtensionManager::updates() const {
    std::vector<ExtensionUpdate> result;
    for (const auto& [id, record] : records_) {
        if (record->info.source == ExtensionSource::builtin) {
            continue;
        }
        try {
            const Manifest manifest =
                load_manifest(record->info.folder / manifest_file_name, options_.engine);
            if (manifest.id == id && manifest.version != record->info.manifest->version) {
                result.push_back({id, record->info.manifest->version, manifest.version});
            }
        } catch (const std::exception&) {
            // Unreadable now: nothing to offer.
        }
    }
    return result;
}

std::vector<NeededModule> ExtensionManager::needed_modules() const {
    std::vector<NeededModule> result;
    for (const std::string& owner : document_.owners()) {
        if (owner == model::core_owner || document_.is_owner_enabled(owner)) {
            continue;
        }
        result.push_back({.id = owner,
                          .objects = document_.find_by_owner(owner).size(),
                          .extension = find(owner)});
    }
    return result;
}

std::vector<const ExtensionInfo*> ExtensionManager::extensions() const {
    std::vector<const ExtensionInfo*> result;
    result.reserve(records_.size());
    for (const auto& [id, record] : records_) {
        result.push_back(&record->info);
    }
    return result;
}

const ExtensionInfo* ExtensionManager::find(std::string_view id) const {
    const Record* record = find_record(id);
    return record != nullptr ? &record->info : nullptr;
}

const ExtensionInfo& ExtensionManager::get(std::string_view id) const {
    return get_record(id).info;
}

std::vector<IndexedCommand> ExtensionManager::command_index() const {
    std::vector<IndexedCommand> index;
    for (const auto& [id, record] : records_) {
        for (const CommandContribution& command : record->contributions->commands) {
            index.push_back({&command, &record->info});
        }
    }
    std::stable_sort(index.begin(), index.end(),
                     [](const IndexedCommand& a, const IndexedCommand& b) {
                         return a.command->id < b.command->id;
                     });
    return index;
}

ExtensionManager::Record* ExtensionManager::find_record(std::string_view id) const {
    const auto it = records_.find(id);
    return it == records_.end() ? nullptr : it->second.get();
}

ExtensionManager::Record& ExtensionManager::get_record(std::string_view id) const {
    Record* record = find_record(id);
    if (record == nullptr) {
        throw ExtensionError(ExtensionErrc::unknown_extension,
                             "no module '" + std::string(id) + "' is installed");
    }
    return *record;
}

// --- Enabling ------------------------------------------------------------------------------------

std::vector<std::string> ExtensionManager::resolve(std::string_view id, bool skip_enabled) const {
    enum class Mark : std::uint8_t { visiting, done };
    std::map<std::string, Mark, std::less<>> marks;
    std::vector<std::string> order;
    std::vector<std::string> chain;  // The modules being visited, from `id`.
    const std::string root(id);

    const std::function<void(const Record&)> visit = [&](const Record& record) {
        const ExtensionInfo& info = record.info;
        if ((skip_enabled && info.enabled()) || marks.contains(info.id())) {
            return;
        }
        if (!info.compatible()) {
            if (chain.empty()) {
                throw ExtensionError(ExtensionErrc::incompatible,
                                     "cannot enable '" + info.id() + "': " + info.incompatibility);
            }
            throw ExtensionError(ExtensionErrc::unresolved_dependency,
                                 "cannot enable '" + root + "': it needs '" + info.id() + "' (" +
                                     join(chain, " → ") + " → " + info.id() + "), and " +
                                     info.incompatibility);
        }
        marks.emplace(info.id(), Mark::visiting);
        chain.push_back(info.id());
        std::vector<const Requirement*> requirements;
        for (const Requirement& requirement : info.manifest->requirements) {
            requirements.push_back(&requirement);
        }
        std::sort(requirements.begin(), requirements.end(),
                  [](const Requirement* a, const Requirement* b) { return a->id < b->id; });
        for (const Requirement* requirement : requirements) {
            const std::string path = join(chain, " → ") + " → " + requirement->id;
            const Record* dependency = find_record(requirement->id);
            if (dependency == nullptr) {
                throw ExtensionError(ExtensionErrc::unresolved_dependency,
                                     "cannot enable '" + root + "': module '" + requirement->id +
                                         "' is not installed (" + path + ")");
            }
            const Version& version = dependency->info.manifest->version;
            if (!requirement->range.contains(version)) {
                throw ExtensionError(ExtensionErrc::unresolved_dependency,
                                     "cannot enable '" + root + "': '" + info.id() + "' needs '" +
                                         requirement->id + "' " + requirement->range.to_string() +
                                         ", but version " + version.to_string() +
                                         " is installed (" + path + ")");
            }
            if (const auto mark = marks.find(requirement->id);
                mark != marks.end() && mark->second == Mark::visiting) {
                throw ExtensionError(
                    ExtensionErrc::dependency_cycle,
                    "cannot enable '" + root + "': modules require each other (" + path + ")");
            }
            visit(*dependency);
        }
        chain.pop_back();
        marks[info.id()] = Mark::done;
        order.push_back(info.id());
    };
    visit(get_record(id));
    return order;
}

std::vector<std::string> ExtensionManager::plan_enable(std::string_view id) const {
    return resolve(id, true);
}

void ExtensionManager::check_conflicts(const std::vector<std::string>& plan) const {
    ContributionRegistry trial = registry_;
    for (const std::string& id : plan) {
        const Record& record = get_record(id);
        trial.add(record.contributions);
        for (const CommandContribution& command : record.contributions->commands) {
            if (const command::CommandSpec* spec = bus_.find_command(command.id)) {
                throw ExtensionError(ExtensionErrc::conflict,
                                     "command '" + command.id + "' of module '" + id +
                                         "' is already registered by '" + spec->owner + "'");
            }
        }
        for (const ContextKeyContribution& key : record.contributions->context_keys) {
            if (const context::KeySpec* spec = keys_.find(key.name);
                spec != nullptr && *spec != key.spec()) {
                throw ExtensionError(ExtensionErrc::conflict,
                                     "context key '" + key.name + "' of module '" + id +
                                         "' is already declared by '" + spec->owner + "'");
            }
        }
    }
}

std::vector<std::string> ExtensionManager::enable(std::string_view id, Journaling journaling) {
    check_journaling(journaling);
    std::vector<std::string> plan = plan_enable(id);
    if (!plan.empty()) {
        check_conflicts(plan);
        for (const std::string& module : plan) {
            enable_one(get_record(module));
        }
        if (started_) {
            activate_on(ActivationEvent::startup());
        }
    }
    if (journaling == Journaling::on) {
        journal("enable", id);
    }
    return plan;
}

void ExtensionManager::enable_one(Record& record) {
    const std::string& id = record.info.id();
    registry_.add(record.contributions);
    try {
        for (const ContextKeyContribution& key : record.contributions->context_keys) {
            keys_.declare(key.spec());
        }
        for (const CommandContribution& command : record.contributions->commands) {
            bus_.register_command({.id = command.id, .owner = id, .args = command.args},
                                  [this, owner = id](command::CommandContext& context) {
                                      return run_command(owner, context);
                                  });
        }
        document_.set_owner_enabled(id, true);
    } catch (...) {
        bus_.unregister_owner(id);
        keys_.remove_owner(id);
        registry_.remove_owner(id);
        throw;
    }
    record.info.state = ExtensionState::enabled;
    record.info.activation_error.clear();
    emit(ExtensionEvent::Kind::enabled, id);
}

std::vector<std::string> ExtensionManager::plan_disable(std::string_view id) const {
    const Record& root = get_record(id);
    std::vector<std::string> order;
    if (!root.info.enabled()) {
        return order;
    }
    std::set<std::string, std::less<>> seen;
    const std::function<void(const std::string&)> visit = [&](const std::string& module) {
        if (!seen.insert(module).second) {
            return;
        }
        for (const auto& [other, record] : records_) {
            if (!record->info.enabled()) {
                continue;
            }
            const auto& requirements = record->info.manifest->requirements;
            const bool needs = std::any_of(requirements.begin(), requirements.end(),
                                           [&](const Requirement& r) { return r.id == module; });
            if (needs) {
                visit(other);
            }
        }
        order.push_back(module);
    };
    visit(root.info.id());
    return order;
}

std::vector<std::string> ExtensionManager::disable(std::string_view id, Journaling journaling) {
    check_journaling(journaling);
    std::vector<std::string> plan = plan_disable(id);
    for (const std::string& module : plan) {
        if (get_record(module).running > 0) {
            throw ExtensionError(ExtensionErrc::invalid_state,
                                 "cannot disable '" + module + "' while its command is running");
        }
    }
    for (const std::string& module : plan) {
        disable_one(get_record(module));
    }
    if (journaling == Journaling::on) {
        journal("disable", id);
    }
    return plan;
}

void ExtensionManager::disable_one(Record& record) {
    const std::string& id = record.info.id();
    if (record.instance) {
        record.instance->deactivate();
        record.instance.reset();
    }
    record.host.reset();
    bus_.unregister_owner(id);
    keys_.remove_owner(id);
    registry_.remove_owner(id);
    document_.set_owner_enabled(id, false);
    record.info.state = ExtensionState::disabled;
    record.info.activation_error.clear();
    emit(ExtensionEvent::Kind::deactivated, id);
}

// --- Activation ----------------------------------------------------------------------------------

void ExtensionManager::set_runtime(ExtensionKind kind, std::shared_ptr<ExtensionRuntime> runtime) {
    if (runtime) {
        runtimes_[kind] = std::move(runtime);
    } else {
        runtimes_.erase(kind);
    }
}

void ExtensionManager::startup() {
    started_ = true;
    activate_on(ActivationEvent::startup());
}

std::vector<std::string> ExtensionManager::activate_on(const ActivationEvent& event) {
    std::vector<std::string> activated;
    for (auto& [id, record] : records_) {
        const ExtensionInfo& info = record->info;
        if (info.state != ExtensionState::enabled || !info.activation_error.empty() ||
            !info.manifest->has_code()) {
            continue;
        }
        const auto& events = info.manifest->activation;
        const bool waiting = std::any_of(events.begin(), events.end(), [&](const auto& pattern) {
            return pattern.matches(event);
        });
        if (!waiting) {
            continue;
        }
        try {
            activate_record(*record, activated);
        } catch (const ExtensionError&) {
            // Reported by "extension.activationFailed"; other modules still get the event.
        }
    }
    return activated;
}

void ExtensionManager::activate(std::string_view id) {
    Record& record = get_record(id);
    if (!record.info.enabled()) {
        throw ExtensionError(ExtensionErrc::invalid_state,
                             "module '" + std::string(id) + "' is disabled");
    }
    std::vector<std::string> activated;
    activate_record(record, activated);
}

void ExtensionManager::activate_record(Record& record, std::vector<std::string>& activated) {
    ExtensionInfo& info = record.info;
    if (info.state != ExtensionState::enabled || !info.manifest->has_code()) {
        return;
    }
    const auto fail = [&](const std::string& reason) {
        info.activation_error = reason;
        log::logger("extension")->error("module '{}' failed to activate: {}", info.id(), reason);
        emit(ExtensionEvent::Kind::activation_failed, info.id(), reason);
        throw ExtensionError(ExtensionErrc::activation_failed,
                             "module '" + info.id() + "': " + reason);
    };
    for (const Requirement& requirement : info.manifest->requirements) {
        Record& dependency = get_record(requirement.id);
        try {
            activate_record(dependency, activated);
        } catch (const ExtensionError&) {
            fail("required module '" + requirement.id +
                 "' failed to activate: " + dependency.info.activation_error);
        }
    }
    const ExtensionKind kind = *info.manifest->kind;
    const auto runtime = runtimes_.find(kind);
    if (runtime == runtimes_.end()) {
        fail("no runtime for " + std::string(to_string(kind)) + " modules is available");
    }
    // The runtime may be replaced while it runs; keep this one alive.
    const std::shared_ptr<ExtensionRuntime> code = runtime->second;
    auto host = std::unique_ptr<ExtensionHost>(new ExtensionHost(info, keys_, *this));
    std::unique_ptr<ExtensionInstance> instance;
    try {
        instance = code->activate(info, *host);
    } catch (const std::exception& e) {
        fail(e.what());
    } catch (...) {
        fail("unknown error");
    }
    if (!instance) {
        fail("the runtime returned no code");
    }
    record.host = std::move(host);
    record.instance = std::move(instance);
    info.state = ExtensionState::active;
    info.activation_error.clear();
    info.code_loaded = info.code_loaded || code->keeps_code_loaded();
    activated.push_back(info.id());
    emit(ExtensionEvent::Kind::activated, info.id());
}

command::CommandResult ExtensionManager::run_command(const std::string& owner,
                                                     command::CommandContext& context) {
    Record& record = get_record(owner);
    if (!record.info.enabled()) {
        throw ExtensionError(ExtensionErrc::invalid_state, "module '" + owner + "' is disabled");
    }
    activate_on(ActivationEvent::command(context.spec().id));
    if (record.info.state != ExtensionState::active) {
        if (!record.info.activation_error.empty()) {
            throw ExtensionError(ExtensionErrc::activation_failed,
                                 "module '" + owner + "': " + record.info.activation_error);
        }
        std::vector<std::string> activated;
        activate_record(record, activated);
    }
    struct Running {
        int& count;
        explicit Running(int& counter) : count(++counter) {}
        Running(const Running&) = delete;
        Running& operator=(const Running&) = delete;
        ~Running() { --count; }
    } running(record.running);
    return record.instance->execute(context);
}

// --- Profiles ------------------------------------------------------------------------------------

Profile ExtensionManager::profile(std::string name, nlohmann::json layout) const {
    Profile result;
    result.name = std::move(name);
    result.layout = std::move(layout);
    for (const auto& [id, record] : records_) {
        if (record->info.enabled()) {
            result.extensions.push_back({id, record->info.manifest->version});
        }
    }
    return result;
}

ProfilePlan ExtensionManager::plan_profile(const Profile& profile) const {
    std::set<std::string, std::less<>> keep;
    for (const ProfileEntry& entry : profile.extensions) {
        if (find_record(entry.id) == nullptr) {
            continue;
        }
        try {
            for (std::string& module : resolve(entry.id, false)) {
                keep.insert(std::move(module));
            }
        } catch (const ExtensionError&) {
            // Cannot be enabled: apply_profile reports it.
        }
    }
    ProfilePlan plan;
    for (const auto& [id, record] : records_) {
        const bool kept = keep.contains(id);
        if (kept && !record->info.enabled()) {
            plan.enable.push_back(id);
        } else if (!kept && record->info.enabled()) {
            plan.disable.push_back(id);
        }
    }
    return plan;
}

ProfileReport ExtensionManager::apply_profile(const Profile& profile, Journaling journaling) {
    check_journaling(journaling);
    ProfileReport report;
    std::vector<std::string> wanted;
    for (const ProfileEntry& entry : profile.extensions) {
        const Record* record = find_record(entry.id);
        if (record == nullptr) {
            report.missing.push_back(entry.id);
            continue;
        }
        const Version& installed = record->info.manifest->version;
        if (installed != entry.version) {
            report.version_mismatches.push_back(
                {entry.id, "the profile has version " + entry.version.to_string() + ", version " +
                               installed.to_string() + " is installed"});
        }
        wanted.push_back(entry.id);
    }
    std::set<std::string, std::less<>> keep;
    std::vector<std::string> to_enable;
    for (const std::string& id : wanted) {
        try {
            for (std::string& module : resolve(id, false)) {
                keep.insert(std::move(module));
            }
            to_enable.push_back(id);
        } catch (const ExtensionError& e) {
            report.failed.push_back({id, e.what()});
        }
    }
    // What the kept modules need is kept too, so disabling the rest never cascades into them.
    for (const auto& [id, record] : records_) {
        if (record->info.enabled() && !keep.contains(id)) {
            for (std::string& module : disable(id)) {
                report.disabled.push_back(std::move(module));
            }
        }
    }
    for (const std::string& id : to_enable) {
        try {
            for (std::string& module : enable(id)) {
                report.enabled.push_back(std::move(module));
            }
        } catch (const ExtensionError& e) {
            report.failed.push_back({id, e.what()});
        }
    }
    if (journaling == Journaling::on) {
        for (const std::string& id : report.disabled) {
            journal("disable", id);
        }
        for (const std::string& id : report.enabled) {
            journal("enable", id);
        }
    }
    return report;
}

// --- Journal -------------------------------------------------------------------------------------

void ExtensionManager::check_journaling(Journaling journaling) const {
    if (journaling == Journaling::on && !bus_.is_idle()) {
        throw ExtensionError(ExtensionErrc::invalid_state,
                             "modules cannot be enabled or disabled inside a command or a "
                             "transaction");
    }
}

void ExtensionManager::journal(std::string_view action, std::string_view id) {
    bus_.journal_action("stresslab.extensions." + std::string(action) + "(" +
                        command::python_string(id) + ")");
}

// --- Events --------------------------------------------------------------------------------------

Subscription ExtensionManager::on_event(std::function<void(const ExtensionEvent&)> listener) {
    return events_.subscribe(std::move(listener));
}

void ExtensionManager::emit(ExtensionEvent::Kind kind, const std::string& id, std::string message,
                            std::optional<CrashReport> crash) {
    events_.emit(ExtensionEvent{kind, id, std::move(message), std::move(crash)});
}

}  // namespace fem::extension
