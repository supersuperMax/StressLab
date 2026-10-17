#pragma once

#include "fem/core/l10n/localization.h"
#include "fem/core/settings/settings.h"

#include <QColor>
#include <QString>
#include <cstdint>
#include <string_view>

// Words of the history in the window (task 2.6, docs/interface-design.md §12, "Vocabulary"): by
// default "version", "variant", "label", "put aside"; with the setting "history.gitTerms" the
// words of git ("commit", "branch", "tag", "stash").
namespace fem::shell {

/// The text of a string of the shell; with the setting "history.gitTerms" on, "<key>.git" if the
/// catalogs have it.
[[nodiscard]] QString history_text(const l10n::Localization& localization,
                                   const settings::Settings& settings, std::string_view key,
                                   const l10n::Args& args = {});

/// The name a variant is shown with: the main variant ("main") is "Main variant" ("main" in the
/// terms of git), the others as they are named.
[[nodiscard]] QString variant_title(const l10n::Localization& localization,
                                    const settings::Settings& settings, std::string_view name);

/// The colour of a variant in the graph, on the stage bar and in the switcher: the main variant
/// blue, the others one of a few colours that read on both themes, by their names (the same name
/// always has the same colour). Never the coral of the brand: it is for accents.
[[nodiscard]] QColor variant_color(std::string_view name);

/// Graphite or white, whichever reads on `background`.
[[nodiscard]] QColor text_color_on(const QColor& background);

/// The local time of a version ("17.10.2026 14:05"; the format is the string "history.time").
[[nodiscard]] QString version_time(const l10n::Localization& localization, std::int64_t seconds);

}  // namespace fem::shell
