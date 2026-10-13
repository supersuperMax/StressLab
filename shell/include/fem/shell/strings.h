#pragma once

#include "fem/core/l10n/localization.h"

#include <QString>
#include <string_view>

// Strings of the shell's own interface: catalogs `shell/l10n/<language>.json` built into the shell
// and added to the localization of the application under the owner `fem.shell`.
namespace fem::shell {

inline constexpr std::string_view strings_owner = "fem.shell";

/// Adds the catalogs of the shell to `localization`. Throws L10nError if a built-in catalog is
/// broken (a bug of the build, caught by the tests).
void add_shell_catalogs(l10n::Localization& localization);

/// The text of a shell string in the current language, with the arguments put in.
[[nodiscard]] QString text(const l10n::Localization& localization, std::string_view key,
                           const l10n::Args& args = {});

/// A UTF-8 string of the core as a QString.
[[nodiscard]] QString to_qstring(std::string_view text);

}  // namespace fem::shell
