#pragma once

#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QPixmap>
#include <QSize>
#include <QString>

// The brand kit in the shell (resources/branding/stresslab-kit/guidelines.md). The files are built
// into the shell as they are in the kit, by the paths of cmake/Branding.cmake.
namespace fem::shell::brand {

/// Graphite, the main colour (guidelines §4).
inline const QColor graphite{0x1B, 0x1F, 0x24};
/// Coral, the "hot module": accents only.
inline const QColor coral{0xFF, 0x5A, 0x36};
/// Paper, the light background.
inline const QColor paper{0xF4, 0xF5, 0xF7};

/// The largest icon size, in device pixels, drawn with the small-size symbol (guidelines §6).
inline constexpr int small_symbol_max_size = 32;

/// Resource paths of the kit files.
inline const QString symbol_small_file = QStringLiteral(":/stresslab/brand/symbol-small-color.svg");
inline const QString app_icon_file = QStringLiteral(":/stresslab/brand/app-icon.svg");
inline const QString logo_color_file = QStringLiteral(":/stresslab/brand/logo-color.svg");
inline const QString logo_reversed_file = QStringLiteral(":/stresslab/brand/logo-reversed.svg");

/// An SVG drawn at `size` logical pixels for a screen with `device_pixel_ratio`, keeping its
/// proportions inside the size; a null pixmap if the file cannot be read.
[[nodiscard]] QPixmap render_svg(const QString& file, QSize size, qreal device_pixel_ratio = 1.0);

/// The icon of the application and its windows: sizes up to `small_symbol_max_size` device
/// pixels are the small-size symbol, larger ones the application icon of the kit.
[[nodiscard]] QIcon application_icon();

/// Whether a palette is dark: its window colour is darker than its text.
[[nodiscard]] bool is_dark(const QPalette& palette);

/// The horizontal logo for a background: "color" on a light one, "reversed" on a dark one.
[[nodiscard]] QString logo_file(const QPalette& palette);

}  // namespace fem::shell::brand
