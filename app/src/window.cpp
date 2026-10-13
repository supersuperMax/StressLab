#include "window.h"

#include "fem/core/job/job_keys.h"
#include "fem/core/log/log.h"
#include "fem/core/product.h"
#include "fem/core/user_folders.h"
#include "fem/core/version.h"
#include "fem/shell/brand.h"
#include "fem/shell/main_window.h"
#include "fem/shell/strings.h"
#include "fem/shell/theme.h"
#include "fem/shell/workspaces.h"

#include "log_slot.h"
#include "module_pump.h"

#include <QApplication>
#include <QLocale>
#include <QMetaObject>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace fem::app {

namespace {

/// The first UI language of the system as Qt reads it ("ru-RU"); empty if unknown.
std::string qt_system_language() {
    const QStringList languages = QLocale::system().uiLanguages();
    return languages.isEmpty() ? std::string() : languages.front().toStdString();
}

}  // namespace

int run_window(Environment environment) {
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

    application.install_modules();
    application.enable_all_modules();
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
    shell::MainWindow window(shell::Workbench{.localization = application.localization(),
                                              .workspaces = workspaces,
                                              .extensions = application.extensions(),
                                              .bus = application.bus(),
                                              .keys = application.keys()});
    window.show();

    application.extensions().startup();
    const int code = QApplication::exec();
    pump.drain();
    log->info("the window closed");
    return code;
}

}  // namespace fem::app
