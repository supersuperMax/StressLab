#include "application.h"

#include "fem/core/bindings.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/profile.h"
#include "fem/core/job/local_executor.h"
#include "fem/core/settings/error.h"

#include <algorithm>
#include <exception>
#include <system_error>
#include <utility>

namespace fem::app {

namespace {

namespace fs = std::filesystem;
using extension::ExtensionEvent;

/// A path for messages, UTF-8.
std::string display(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

void log_scan(const extension::ScanReport& report, const std::string& category) {
    const auto log = log::logger(category);
    for (const extension::ScanProblem& problem : report.problems) {
        log->error("the module in '{}' cannot be installed: {}", display(problem.folder),
                   problem.message);
    }
}

/// The size of the cache of results follows the setting "results.cacheLimit" (megabytes; the core
/// does not know the storage, so the binding lives here).
Subscription bind_result_cache_limit(settings::Settings& settings, storage::ResultCache& results) {
    const auto apply = [&settings, &results] {
        const auto megabytes = settings.get<std::int64_t>(settings::keys::result_cache_limit);
        results.set_limit(megabytes <= 0 ? 0 : static_cast<std::uint64_t>(megabytes) * 1024 * 1024);
    };
    apply();
    return settings.on_changed([apply](std::string_view key) {
        if (key == settings::keys::result_cache_limit) {
            apply();
        }
    });
}

}  // namespace

Application::Application(const Environment& environment, std::string log_category)
    : log_category_(std::move(log_category)),
      builtin_extensions_(environment.builtin_extensions),
      bus_(document_),
      core_keys_(bus_, keys_),
      manager_(document_, bus_, keys_),
      project_(bus_, [this](std::string_view id) {
          const extension::ExtensionInfo* info = manager_.find(id);
          return info != nullptr ? info->manifest->version.to_string() : std::string();
      }) {
    const auto log = log::logger(log_category_);
    bindings_.push_back(bind_settings(manager_, settings_));
    bindings_.push_back(bind_catalogs(manager_, localization_));
    bindings_.push_back(bind_language(settings_, localization_, environment.system_language));
    bindings_.push_back(bind_undo_limit(settings_, bus_));
    bindings_.push_back(bind_result_cache_limit(settings_, project_.results()));
    if (environment.user_settings) {
        try {
            (void)settings_.load_file(settings::Level::user, *environment.user_settings);
        } catch (const settings::SettingsError& error) {
            log->warn("the settings of the user are not used: {}", error.what());
        }
    }

    // Python and process modules run in processes of their own; the extension host is this
    // executable with its embedded Python.
    extension::HostRuntimeOptions runtime_options;
    runtime_options.python =
        extension::python_launcher(environment.executable, {"--python", "-m", "stresslab_host"});
    runtime_ = std::make_shared<extension::HostRuntime>(bus_, runtime_options);
    manager_.set_runtime(extension::ExtensionKind::python, runtime_);
    manager_.set_runtime(extension::ExtensionKind::process, runtime_);
    jobs_.add_executor(std::make_shared<job::LocalExecutor>());
    events_ = manager_.on_event([this](const ExtensionEvent& event) {
        const auto log = log::logger(log_category_);
        switch (event.kind) {
            case ExtensionEvent::Kind::installed:
                log->debug("module '{}' installed", event.id);
                break;
            case ExtensionEvent::Kind::uninstalled:
                log->info("module '{}' uninstalled", event.id);
                break;
            case ExtensionEvent::Kind::updated:
                log->info("module '{}' updated", event.id);
                break;
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
                    if (const extension::ExtensionInfo* info = manager_.find(event.id)) {
                        log->warn("{}",
                                  extension::crash_message(*info, *event.crash, localization_));
                    }
                }
                break;
        }
    });
}

Application::~Application() = default;

void Application::follow_log_level(log::Logging& logging) {
    bindings_.push_back(bind_log_level(settings_, logging));
}

void Application::install_modules(const std::vector<fs::path>& folders) {
    std::error_code error;
    if (!builtin_extensions_.empty() && fs::is_directory(builtin_extensions_, error)) {
        log_scan(manager_.scan(builtin_extensions_, extension::ExtensionSource::builtin),
                 log_category_);
    }
    for (const fs::path& folder : folders) {
        log_scan(manager_.scan(folder, extension::ExtensionSource::local), log_category_);
    }
}

void Application::enable_all_modules(const std::vector<std::string>& except) {
    const auto log = log::logger(log_category_);
    for (const extension::ExtensionInfo* info : manager_.extensions()) {
        if (info->enabled() ||
            std::find(except.begin(), except.end(), info->id()) != except.end()) {
            continue;
        }
        if (!info->compatible()) {
            log->warn("module '{}' is not enabled: {}", info->id(), info->incompatibility);
            continue;
        }
        try {
            (void)manager_.enable(info->id());
        } catch (const extension::ExtensionError& error) {
            log->error("module '{}' cannot be enabled: {}", info->id(), error.what());
        }
    }
}

void Application::enable_user_modules(const extension::UserModules& modules) {
    const auto log = log::logger(log_category_);
    for (const fs::path& folder : modules.local) {
        try {
            (void)manager_.install(folder, extension::ExtensionSource::local);
        } catch (const std::exception& error) {
            log->error("the module in '{}' cannot be installed: {}", display(folder), error.what());
        }
    }
    enable_all_modules(modules.disabled);
}

bool Application::enable_profile(const fs::path& file) {
    const auto log = log::logger(log_category_);
    extension::Profile profile;
    try {
        profile = extension::load_profile(file);
    } catch (const std::exception& error) {
        log->error("the profile '{}' cannot be read: {}", display(file), error.what());
        return false;
    }
    const extension::ProfileReport report = manager_.apply_profile(profile);
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

}  // namespace fem::app
