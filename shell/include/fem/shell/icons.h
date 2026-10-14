#pragma once

#include "fem/core/extension/manager.h"
#include "fem/core/form/form.h"

#include <QByteArray>
#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QString>
#include <string_view>

// Icons of modules (docs/adr/0013-shell-contributions.md): monochrome SVG drawn in a colour of the
// palette, so that they follow the light and the dark theme; a module without an icon of its own
// gets the default one, a tile of the brand symbol (resources/branding/stresslab-kit/guidelines.md
// §8).
namespace fem::shell {

/// The colour an icon is drawn in for a mode and state of `palette`: the text colour normally, the
/// muted text colour when disabled, the accent when selected or checked.
[[nodiscard]] QColor icon_color(const QPalette& palette, QIcon::Mode mode, QIcon::State state);

/// An icon from a monochrome SVG: whatever its colours, its shape is drawn in `icon_color` of the
/// palette of the application at the moment it is drawn. A null icon if `svg` is not an SVG.
[[nodiscard]] QIcon tinted_svg_icon(const QByteArray& svg);
/// The same for a file; a null icon if it cannot be read.
[[nodiscard]] QIcon tinted_svg_icon_file(const QString& file);

/// The SVG of the default module icon: the "hot" tile of the small-size symbol of the brand kit,
/// the path taken from the kit file as it is, framed to the tile.
[[nodiscard]] QByteArray default_module_icon_svg();
/// The default module icon, tinted like `tinted_svg_icon`.
[[nodiscard]] QIcon default_module_icon();

/// The mark of a status in the model tree and in forms: a triangle with "!" for a warning, a circle
/// with "×" for an error, drawn in fixed colours that read on both themes (in the text colour of
/// a selection on a selected row); a null icon for ok.
[[nodiscard]] QIcon status_icon(form::Severity severity);

/// The icon a module names in a contribution: the id of an `icons` contribution, or a path in the
/// folder of the module. With `fallback`, the default module icon when `icon` is empty or the
/// file cannot be read; otherwise a null icon then.
[[nodiscard]] QIcon module_icon(const extension::ExtensionManager& extensions,
                                std::string_view owner, std::string_view icon, bool fallback);

}  // namespace fem::shell
