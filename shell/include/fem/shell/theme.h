#pragma once

#include <QPalette>
#include <Qt>

class QApplication;

// Light and dark themes of the shell (ADR 0012): the Fusion style on every OS with palettes of the
// brand colours, so that the window looks the same on Windows, macOS and Linux. By default the
// theme follows the colour scheme of the system and changes with it.
namespace fem::shell {

/// The palette of a colour scheme; `Unknown` is light. Light: paper windows, white fields,
/// graphite text. Dark: graphite windows, paper text. Both: coral accent and selection with
/// graphite text on it.
[[nodiscard]] QPalette theme_palette(Qt::ColorScheme scheme);

/// Sets the Fusion style (once) and the palette of `scheme` on the application.
void apply_theme(QApplication& application, Qt::ColorScheme scheme);

/// Applies the theme of the system now and whenever the system changes its colour scheme.
void follow_system_theme(QApplication& application);

}  // namespace fem::shell
