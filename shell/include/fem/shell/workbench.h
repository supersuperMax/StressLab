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
#include "fem/storage/session.h"

#include <QString>
#include <cstdint>
#include <functional>

class QWidget;

namespace fem::shell {

class ConsoleEngine;

/// What to do with unsaved changes of the project before it is closed.
enum class UnsavedChanges : std::uint8_t { save, discard, cancel };

/// What to do with changes not in the history before another variant or version opens (task
/// 2.6): commit them as a version, put them aside, or stay.
enum class PendingChanges : std::uint8_t { commit, stash, cancel };

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
    /// The project folder of the document (task 2.1); nullptr: the window cannot open or save.
    storage::ProjectSession* project = nullptr;
    /// Asks what to do with the unsaved changes of the project of this name (parent, name).
    /// Empty: a message box of the system; tests give their own.
    std::function<UnsavedChanges(QWidget*, const QString&)> ask_unsaved = {};
    /// Asks what to do with the changes not in the history before another variant or version
    /// opens (parent, the message of a version to commit them as, which the answer may change).
    /// Empty: a dialog of the shell; tests give their own.
    std::function<PendingChanges(QWidget*, QString& message)> ask_pending = {};
    /// Asks for a name: of a new variant, of a label (parent, title, label of the field, the text
    /// it starts with); an empty answer is "cancelled". Empty: an input dialog; tests give their
    /// own.
    std::function<QString(QWidget*, const QString& title, const QString& label,
                          const QString& text)>
        ask_name = {};
};

}  // namespace fem::shell
