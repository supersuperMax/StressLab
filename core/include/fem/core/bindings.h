#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/extension/manager.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/log/log.h"
#include "fem/core/settings/settings.h"
#include "fem/core/signal.h"

// Connections between the parts of the core that do not know each other: the application makes
// them at startup. Each applies the current state at once and keeps following it while the
// returned subscription lives; the connected objects must outlive it.
namespace fem {

/// Settings get the schemas of a module when it is enabled and lose them when it is disabled.
[[nodiscard]] Subscription bind_settings(extension::ExtensionManager& manager,
                                         settings::Settings& settings);

/// Localization gets the catalogs (`contributes.localization`) of a module when it is enabled and
/// loses them when it is disabled. Catalog files are read from the module's folder; one that
/// cannot be loaded is reported in `Localization::issues()` and the log, and the module is
/// enabled anyway.
[[nodiscard]] Subscription bind_catalogs(extension::ExtensionManager& manager,
                                         l10n::Localization& localization);

/// The language follows the setting "general.language" ("" is the language of the system:
/// `system_language`, or `l10n::system_language()` if that is empty; the window passes the one
/// it reads from Qt, which also knows it on macOS).
[[nodiscard]] Subscription bind_language(settings::Settings& settings,
                                         l10n::Localization& localization,
                                         std::string system_language = {});

/// The undo stack is limited by the setting "history.undoLimit".
[[nodiscard]] Subscription bind_undo_limit(settings::Settings& settings, command::CommandBus& bus);

/// The level of the log follows the setting "log.level".
[[nodiscard]] Subscription bind_log_level(settings::Settings& settings, log::Logging& logging);

}  // namespace fem
