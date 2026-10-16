#include "window.h"

#include "fem/core/command/journal.h"
#include "fem/core/extension/profile.h"
#include "fem/core/extension/user_modules.h"
#include "fem/core/job/job_keys.h"
#include "fem/core/log/log.h"
#include "fem/core/product.h"
#include "fem/core/user_folders.h"
#include "fem/core/version.h"
#include "fem/python/interpreter.h"
#include "fem/shell/brand.h"
#include "fem/shell/main_window.h"
#include "fem/shell/project_actions.h"
#include "fem/shell/strings.h"
#include "fem/shell/theme.h"
#include "fem/shell/workspaces.h"

#include "console_engine.h"
#include "log_slot.h"
#include "module_pump.h"

#include <QApplication>
#include <QLocale>
#include <QMetaObject>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace fem::app {

namespace {

/// The first UI language of the system as Qt reads it ("ru-RU"); empty if unknown.
std::string qt_system_language() {
    const QStringList languages = QLocale::system().uiLanguages();
    return languages.isEmpty() ? std::string() : languages.front().toStdString();
}

}  // namespace

int run_window(Environment environment, const std::optional<std::filesystem::path>& project) {
    // Qt reads the command line of the process itself where it matters (Windows); the window
    // takes no arguments of its own yet.
    int argc = 1;
    std::string program = shell::to_qstring(kProductName).toLower().toStdString();
    char* argv[] = {program.data(), nullptr};
    QApplication::setApplicationName(shell::to_qstring(kProductName));
    QApplication::setApplicationVersion(shell::to_qstring(fem::version()));
    // Wayland and the desktop of Linux match the window to stresslab.desktop by this name.
    QApplication::setDesktopFileName(QStringLiteral("stresslab"));
    QApplication qt(argc, argv);
    QApplication::setWindowIcon(shell::brand::application_icon());
    shell::follow_system_theme(qt);

    // A log file of its own: other windows may be running.
    std::optional<LogSlot> slot;
    log::LogOptions log_options{.console = log::Console::none};
    try {
        const UserFolders folders = user_folders();
        slot = take_log_slot(folders.logs);
        if (slot) {
            log_options.folder = folders.logs;
            log_options.file_name = slot->file_name;
        }
    } catch (const std::exception&) {
        // No home folder: no log file.
    }
    log::Logging logging(log_options);
    const auto log = log::logger("app");
    log->info("{} {}, window", kProductName, fem::version());

    if (environment.system_language.empty()) {
        environment.system_language = qt_system_language();
    }
    Application application(environment, "app");
    application.follow_log_level(logging);
    shell::add_shell_catalogs(application.localization());

    // What the user installed from folders and disabled in the module manager.
    std::filesystem::path modules_file;
    std::filesystem::path profiles_folder;
    try {
        modules_file = user_folders().config / "modules.json";
        profiles_folder = user_folders().config / "profiles";
    } catch (const std::exception&) {
        // No home folder: the changes live as long as the window.
    }
    extension::UserModules user_state;
    std::error_code missing;
    if (!modules_file.empty() && std::filesystem::exists(modules_file, missing)) {
        try {
            user_state = extension::load_user_modules(modules_file);
        } catch (const std::exception& error) {
            log->warn("the state of the modules is not used: {}", error.what());
        }
    }
    application.install_modules();
    application.enable_user_modules(user_state);
    // What modules ask outside their commands is done in the event loop, between two events.
    const auto call_soon = [&qt](std::function<void()> work) {
        QMetaObject::invokeMethod(&qt, std::move(work), Qt::QueuedConnection);
    };
    ModulePump pump(application.runtime(), application.bus(), call_soon);
    // Jobs change on their threads; their keys change in the event loop.
    const job::JobKeys job_keys(application.jobs(), application.keys(), call_soon);

    std::filesystem::path workspace_file;
    try {
        workspace_file = user_folders().config / "workspaces.json";
    } catch (const std::exception&) {
        // No home folder: layouts live as long as the window.
    }
    shell::WorkspaceStore workspaces =
        workspace_file.empty() ? shell::WorkspaceStore() : shell::WorkspaceStore(workspace_file);
    // The settings form saves what it changes at once.
    std::function<void()> save_settings;
    if (environment.user_settings) {
        save_settings = [&application, file = *environment.user_settings] {
            application.settings().save_file(settings::Level::user, file);
        };
    }
    // So does the module manager.
    std::function<void()> save_modules;
    if (!modules_file.empty()) {
        save_modules = [&application, &log, file = modules_file] {
            try {
                extension::save_user_modules(file,
                                             extension::user_modules(application.extensions()));
            } catch (const std::exception& error) {
                log->error("the state of the modules cannot be saved: {}", error.what());
            }
        };
    }
    std::optional<extension::ProfileLibrary> profiles;
    if (!profiles_folder.empty()) {
        profiles.emplace(profiles_folder);
    }

    // The journal of the session and the Python console, on the document of the window.
    command::SessionJournal journal(application.bus());
    std::optional<python::Interpreter> python;
    std::unique_ptr<PythonConsoleEngine> console;
    std::string console_unavailable;
    try {
        python.emplace();
        python->attach(application.bus(), application.keys(), &application.extensions(),
                       &application.project(), &application.localization());
        console = std::make_unique<PythonConsoleEngine>();
    } catch (const std::exception& error) {
        log->error("the Python console is not available: {}", error.what());
        console_unavailable = error.what();
    }

    int code = 0;
    {
        shell::MainWindow window(shell::Workbench{.localization = application.localization(),
                                                  .workspaces = workspaces,
                                                  .extensions = application.extensions(),
                                                  .bus = application.bus(),
                                                  .keys = application.keys(),
                                                  .settings = application.settings(),
                                                  .save_settings = std::move(save_settings),
                                                  .console = console.get(),
                                                  .console_unavailable = console_unavailable,
                                                  .journal = &journal,
                                                  .profiles = profiles ? &*profiles : nullptr,
                                                  .save_modules = std::move(save_modules),
                                                  .project = &application.project()});
        window.show();

        application.extensions().startup();
        // `stresslab wing.slproj`: a failure is reported in the window like one from the menu.
        if (project) {
            (void)window.project().open(*project);
        }
        window.check_required_modules();
        code = QApplication::exec();
        pump.drain();
    }
    log->info("the window closed");
    console.reset();
    if (python) {
        python->detach();
    }
    return code;
}

}  // namespace fem::app
