#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/core_keys.h"
#include "fem/core/extension/host_runtime.h"
#include "fem/core/extension/manager.h"
#include "fem/core/job/job_manager.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/log/log.h"
#include "fem/core/model/document.h"
#include "fem/core/settings/settings.h"
#include "fem/core/signal.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fem::app {

/// What the application takes from its surroundings rather than from the command line.
struct Environment {
    /// The executable of the application: it also runs the extension host of python modules
    /// (`stresslab --python -m stresslab_host`).
    std::filesystem::path executable;
    /// The folder of the modules shipped with the application; empty or missing: none.
    std::filesystem::path builtin_extensions;
    /// The settings file of the user, if known; a missing file is no error.
    std::optional<std::filesystem::path> user_settings;
    /// The language of the system as the window reads it from Qt (on macOS an application
    /// started from Finder has no LANG); empty: `l10n::system_language()`.
    std::string system_language;
};

/// The parts of the application that the window and the headless run share (ADR 0011,
/// "Consequences"; ADR 0012): the document with its Command Bus and context keys, the settings of
/// the user, the localization, the Extension Manager whose python and process modules run in
/// processes of their own, and the Job Manager with the executor of this computer. Settings and
/// catalogs follow the enabled modules; the language and the undo limit follow the settings.
/// Messages go to the logger of `log_category`.
///
/// Lives on the thread of the model; what modules ask outside their commands waits for
/// `runtime().poll()`, which the owner calls from its loop.
class Application {
public:
    Application(const Environment& environment, std::string log_category);
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;
    ~Application();

    [[nodiscard]] model::Document& document() noexcept { return document_; }
    [[nodiscard]] command::CommandBus& bus() noexcept { return bus_; }
    [[nodiscard]] context::ContextKeys& keys() noexcept { return keys_; }
    [[nodiscard]] settings::Settings& settings() noexcept { return settings_; }
    [[nodiscard]] l10n::Localization& localization() noexcept { return localization_; }
    [[nodiscard]] extension::ExtensionManager& extensions() noexcept { return manager_; }
    [[nodiscard]] extension::HostRuntime& runtime() noexcept { return *runtime_; }
    /// Jobs of solvers and other programs. The keys `job.*` follow it where the owner of the loop
    /// of the thread of the model sets up `job::JobKeys`.
    [[nodiscard]] job::JobManager& jobs() noexcept { return jobs_; }

    /// The level of `logging` follows the setting "log.level" from now on.
    void follow_log_level(log::Logging& logging);

    /// Installs the built-in modules and then the modules in the subfolders of `folders`, in this
    /// order; modules that cannot be installed are logged.
    void install_modules(const std::vector<std::filesystem::path>& folders = {});
    /// Enables every installed module that this platform can run; failures are logged.
    void enable_all_modules();
    /// Enables the modules of a profile; false (and logged) if the profile cannot be read.
    bool enable_profile(const std::filesystem::path& file);

private:
    std::string log_category_;
    std::filesystem::path builtin_extensions_;
    model::Document document_;
    command::CommandBus bus_;
    context::ContextKeys keys_;
    context::CoreKeys core_keys_;
    settings::Settings settings_;
    l10n::Localization localization_;
    extension::ExtensionManager manager_;
    std::shared_ptr<extension::HostRuntime> runtime_;
    job::JobManager jobs_;
    std::vector<Subscription> bindings_;
    Subscription events_;
};

}  // namespace fem::app
