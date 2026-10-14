#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/extension/manager.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/settings/settings.h"
#include "fem/shell/workspaces.h"

#include <functional>

namespace fem::shell {

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
};

}  // namespace fem::shell
