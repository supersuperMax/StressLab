#include "headless.h"

#include "fem/core/bindings.h"
#include "fem/core/command/command_bus.h"
#include "fem/core/command/journal.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/core_keys.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/host_runtime.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/profile.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/log/log.h"
#include "fem/core/model/document.h"
#include "fem/core/product.h"
#include "fem/core/settings/error.h"
#include "fem/core/settings/settings.h"
#include "fem/core/version.h"
#include "fem/python/interpreter.h"

#include "module_pump.h"

#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fem::app {

namespace {

namespace fs = std::filesystem;
using extension::ExtensionEvent;

/// A path for messages, UTF-8.
std::string display(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

void print_error(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void log_event(const ExtensionEvent& event, const extension::ExtensionManager& manager,
               const l10n::Localization& localization) {
    const auto log = log::logger("headless");
    switch (event.kind) {
        case ExtensionEvent::Kind::enabled:
            log->info("module '{}' enabled", event.id);
            break;
        case ExtensionEvent::Kind::activated:
            log->info("module '{}' activated", event.id);
            break;
        case ExtensionEvent::Kind::activation_failed:
            log->error("module '{}' could not be activated: {}", event.id, event.message);
            break;
        case ExtensionEvent::Kind::deactivated:
            log->info("module '{}' disabled", event.id);
            break;
        case ExtensionEvent::Kind::crashed:
            // The details are logged by the manager; this is what the user is told.
            if (event.crash) {
                if (const extension::ExtensionInfo* info = manager.find(event.id)) {
                    log->warn("{}", extension::crash_message(*info, *event.crash, localization));
                }
            }
            break;
    }
}

void log_scan(const extension::ScanReport& report) {
    const auto log = log::logger("headless");
    for (const extension::ScanProblem& problem : report.problems) {
        log->error("the module in '{}' cannot be installed: {}", display(problem.folder),
                   problem.message);
    }
}

/// Enables every installed module that this platform can run.
void enable_all(extension::ExtensionManager& manager) {
    const auto log = log::logger("headless");
    for (const extension::ExtensionInfo* info : manager.extensions()) {
        if (info->enabled()) {
            continue;
        }
        if (!info->compatible()) {
            log->warn("module '{}' is not enabled: {}", info->id(), info->incompatibility);
            continue;
        }
        try {
            (void)manager.enable(info->id());
        } catch (const extension::ExtensionError& error) {
            log->error("module '{}' cannot be enabled: {}", info->id(), error.what());
        }
    }
}

/// Applies a profile; false if it cannot be read.
bool enable_profile(extension::ExtensionManager& manager, const fs::path& file) {
    const auto log = log::logger("headless");
    extension::Profile profile;
    try {
        profile = extension::load_profile(file);
    } catch (const std::exception& error) {
        log->error("the profile '{}' cannot be read: {}", display(file), error.what());
        return false;
    }
    const extension::ProfileReport report = manager.apply_profile(profile);
    for (const std::string& id : report.missing) {
        log->error("module '{}' of the profile is not installed", id);
    }
    for (const extension::ProfileIssue& issue : report.failed) {
        log->error("module '{}' of the profile cannot be enabled: {}", issue.id, issue.message);
    }
    for (const extension::ProfileIssue& issue : report.version_mismatches) {
        log->warn("module '{}': {}", issue.id, issue.message);
    }
    return true;
}

/// The options of the log: the standard output and, with --log-file, a file that grows.
log::LogOptions log_options(const HeadlessOptions& options) {
    log::LogOptions result{.level = options.log_level.value_or(log::Level::info),
                           .console = log::Console::standard_output};
    if (options.log_file) {
        const fs::path file = fs::absolute(*options.log_file);
        result.folder = file.parent_path();
        result.file_name = display(file.filename());
        result.rotate_on_open = false;
    }
    return result;
}

int run(const HeadlessOptions& options, const HeadlessEnvironment& environment) {
    log::Logging logging(log_options(options));
    const auto log = log::logger("headless");

    model::Document document;
    command::CommandBus bus(document);
    context::ContextKeys keys;
    const context::CoreKeys core_keys(bus, keys);
    settings::Settings settings;
    l10n::Localization localization;
    extension::ExtensionManager manager(document, bus, keys);
    std::vector<Subscription> bindings;
    bindings.push_back(bind_settings(manager, settings));
    bindings.push_back(bind_catalogs(manager, localization));
    bindings.push_back(bind_language(settings, localization));
    bindings.push_back(bind_undo_limit(settings, bus));
    if (!options.log_level) {
        bindings.push_back(bind_log_level(settings, logging));
    }
    if (environment.user_settings) {
        try {
            (void)settings.load_file(settings::Level::user, *environment.user_settings);
        } catch (const settings::SettingsError& error) {
            log->warn("the settings of the user are not used: {}", error.what());
        }
    }
    const auto text = [&](std::string_view key, const l10n::Args& args = {}) {
        return localization.text("fem.core", key, args);
    };

    // What the command line names must exist: a typo is a usage error, not a failed script.
    std::error_code error;
    if (!fs::is_regular_file(options.script, error)) {
        print_error(text("app.scriptNotFound", {{"file", display(options.script)}}));
        return exit_usage;
    }
    for (const fs::path& folder : options.extension_folders) {
        if (!fs::is_directory(folder, error)) {
            print_error(text("app.folderNotFound", {{"folder", display(folder)}}));
            return exit_usage;
        }
    }
    if (options.profile && !fs::is_regular_file(*options.profile, error)) {
        print_error(text("app.fileNotFound", {{"file", display(*options.profile)}}));
        return exit_usage;
    }

    log->info("{} {}, headless: {}", kProductName, version(), display(options.script));

    // Modules: python and process modules run in processes of their own; the extension host is
    // this executable with its embedded Python.
    extension::HostRuntimeOptions runtime_options;
    runtime_options.python =
        extension::python_launcher(environment.executable, {"--python", "-m", "stresslab_host"});
    const auto runtime = std::make_shared<extension::HostRuntime>(bus, runtime_options);
    manager.set_runtime(extension::ExtensionKind::python, runtime);
    manager.set_runtime(extension::ExtensionKind::process, runtime);
    const Subscription events = manager.on_event(
        [&](const ExtensionEvent& event) { log_event(event, manager, localization); });
    if (!environment.builtin_extensions.empty() &&
        fs::is_directory(environment.builtin_extensions, error)) {
        log_scan(manager.scan(environment.builtin_extensions, extension::ExtensionSource::builtin));
    }
    for (const fs::path& folder : options.extension_folders) {
        log_scan(manager.scan(folder, extension::ExtensionSource::local));
    }
    if (options.profile) {
        if (!enable_profile(manager, *options.profile)) {
            return exit_failure;
        }
    } else {
        enable_all(manager);
    }

    std::optional<command::FileJournal> journal;
    Subscription journaling;
    if (options.journal) {
        journal.emplace(*options.journal);
        journaling = bus.on_journal([&](std::string_view entry) { journal->write(entry); });
    }

    python::Interpreter python;
    python.attach(bus, keys);
    // Lines of the script and of the log go to the standard output in the order they happen.
    python.run_string("import sys\nsys.stdout.reconfigure(line_buffering=True)\n");
    ModulePump pump(*runtime, bus, python);

    manager.startup();
    int code = python.run_script(options.script, options.script_args);
    pump.drain();
    log->info("the script finished with exit code {}", code);

    if (journal && !journal->good()) {
        log->error("the journal '{}' could not be written", display(journal->path()));
        if (code == exit_success) {
            code = exit_failure;
        }
    }
    python.detach();
    return code;
}

}  // namespace

int run_headless(const HeadlessOptions& options, const HeadlessEnvironment& environment) {
    try {
        return run(options, environment);
    } catch (const std::exception& error) {
        // The log may be gone already; the standard error stream is always there.
        print_error(error.what());
        return exit_failure;
    }
}

}  // namespace fem::app
