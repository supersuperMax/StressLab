#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/command/journal.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/profile.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/settings/settings.h"
#include "fem/shell/file_dialogs.h"
#include "fem/shell/workspaces.h"

#include <functional>

namespace fem::shell {

class ConsoleEngine;

/// What the window works with: the parts of the application it shows and drives. All of them
/// outlive the window. The localization has the catalogs of the shell (`add_shell_catalogs`) and,
/// like the settings, follows the enabled modules (`fem::bind_catalogs`).
struct Workbench {
    l10n::Localization& localization;
    WorkspaceStore& workspaces;
    extension::ExtensionManager& extensions;
    command::CommandBus& bus;
    context::ContextKeys& keys;
    settings::Settings& settings;
    /// Writes the settings of the user where the application keeps them; called after the
    /// settings form changed them. Empty: they live as long as the application (tests).
    std::function<void()> save_settings = {};
    /// The Python console (task 1.14); nullptr: Python is not available, and
    /// `console_unavailable` says why.
    ConsoleEngine* console = nullptr;
    std::string console_unavailable = {};
    /// The journal of the session that the "Journal" tab shows and saves as a script; nullptr:
    /// the tab stays empty.
    command::SessionJournal* journal = nullptr;
    /// The profiles of the user; nullptr: the module manager offers none.
    extension::ProfileLibrary* profiles = nullptr;
    /// Writes what the user changed in the module manager (`extension::user_modules`) where the
    /// application keeps it; called after a module was enabled, disabled, installed, uninstalled
    /// or updated. Empty: nothing is kept (tests).
    std::function<void()> save_modules = {};
    /// How the window asks for files and folders: the dialogs of the system unless tests give
    /// their own.
    FileDialogs dialogs = system_file_dialogs();
};

}  // namespace fem::shell
