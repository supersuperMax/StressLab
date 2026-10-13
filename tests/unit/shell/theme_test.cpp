#include "fem/shell/theme.h"

#include "fem/shell/brand.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QStyle>
#include <algorithm>
#include <cmath>

namespace {

using fem::shell::theme_palette;
namespace brand = fem::shell::brand;

/// Relative luminance of WCAG 2.
double luminance(const QColor& color) {
    const auto channel = [](double value) {
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
           0.0722 * channel(color.blueF());
}

double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

TEST(ShellTheme, LightIsPaperWithGraphiteText) {
    const QPalette palette = theme_palette(Qt::ColorScheme::Light);
    EXPECT_EQ(palette.color(QPalette::Window), brand::paper);
    EXPECT_EQ(palette.color(QPalette::WindowText), brand::graphite);
    EXPECT_EQ(palette.color(QPalette::Text), brand::graphite);
    EXPECT_FALSE(brand::is_dark(palette));
}

TEST(ShellTheme, DarkIsGraphiteWithPaperText) {
    const QPalette palette = theme_palette(Qt::ColorScheme::Dark);
    EXPECT_EQ(palette.color(QPalette::Window), brand::graphite);
    EXPECT_EQ(palette.color(QPalette::WindowText), brand::paper);
    EXPECT_EQ(palette.color(QPalette::Text), brand::paper);
    EXPECT_TRUE(brand::is_dark(palette));
}

TEST(ShellTheme, AnUnknownSchemeIsLight) {
    EXPECT_EQ(theme_palette(Qt::ColorScheme::Unknown), theme_palette(Qt::ColorScheme::Light));
}

TEST(ShellTheme, CoralIsTheAccentOfBothThemes) {
    for (const Qt::ColorScheme scheme : {Qt::ColorScheme::Light, Qt::ColorScheme::Dark}) {
        const QPalette palette = theme_palette(scheme);
        EXPECT_EQ(palette.color(QPalette::Accent), QColor(0xFF, 0x5A, 0x36));
        EXPECT_EQ(palette.color(QPalette::Highlight), brand::coral);
        EXPECT_EQ(palette.color(QPalette::HighlightedText), brand::graphite);
    }
}

TEST(ShellTheme, TextIsReadableOnEveryBackground) {
    for (const Qt::ColorScheme scheme : {Qt::ColorScheme::Light, Qt::ColorScheme::Dark}) {
        const QPalette p = theme_palette(scheme);
        // WCAG AA for normal text.
        EXPECT_GE(contrast(p.color(QPalette::WindowText), p.color(QPalette::Window)), 4.5);
        EXPECT_GE(contrast(p.color(QPalette::Text), p.color(QPalette::Base)), 4.5);
        EXPECT_GE(contrast(p.color(QPalette::ButtonText), p.color(QPalette::Button)), 4.5);
        EXPECT_GE(contrast(p.color(QPalette::HighlightedText), p.color(QPalette::Highlight)), 4.5);
        EXPECT_GE(contrast(p.color(QPalette::ToolTipText), p.color(QPalette::ToolTipBase)), 4.5);
        // Hints are muted, yet readable (AA for large text).
        EXPECT_GE(contrast(p.color(QPalette::PlaceholderText), p.color(QPalette::Base)), 3.0);
    }
}

TEST(ShellTheme, IsAppliedWithTheFusionStyle) {
    auto& application = *qApp;
    fem::shell::apply_theme(application, Qt::ColorScheme::Dark);
    EXPECT_EQ(QApplication::style()->name().toLower(), "fusion");
    EXPECT_EQ(QApplication::palette().color(QPalette::Window), brand::graphite);
    fem::shell::apply_theme(application, Qt::ColorScheme::Light);
    EXPECT_EQ(QApplication::palette().color(QPalette::Window), brand::paper);
}

}  // namespace
