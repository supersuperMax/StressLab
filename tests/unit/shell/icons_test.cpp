// Icons of modules: monochrome SVG in the colours of the theme, and the default icon from the tile
// of the brand symbol (task 1.12).

#include "fem/shell/icons.h"

#include "fem/shell/brand.h"
#include "fem/shell/theme.h"

#include "shell_test_support.h"
#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <filesystem>

namespace {

using fem::shell::icon_color;
using fem::shell::theme_palette;

// A red square in the middle: whatever its colour, the icon draws it in the colour of the theme.
const QByteArray square = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">
<rect x="4" y="4" width="16" height="16" fill="#ff0000"/></svg>)";

QColor middle(const QIcon& icon, QIcon::Mode mode = QIcon::Normal,
              QIcon::State state = QIcon::Off) {
    const QImage image = icon.pixmap(QSize(24, 24), 1.0, mode, state).toImage();
    return image.pixelColor(image.width() / 2, image.height() / 2);
}

class ShellIcons : public ::testing::Test {
protected:
    // Each test may run in a process of its own (ctest): the theme is set here, not by others.
    void SetUp() override { fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light); }
    void TearDown() override { fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light); }
};

TEST_F(ShellIcons, AMonochromeIconTakesTheColoursOfTheTheme) {
    const QIcon icon = fem::shell::tinted_svg_icon(square);
    ASSERT_FALSE(icon.isNull());
    for (const Qt::ColorScheme scheme : {Qt::ColorScheme::Light, Qt::ColorScheme::Dark}) {
        fem::shell::apply_theme(*qApp, scheme);
        const QPalette palette = theme_palette(scheme);
        EXPECT_EQ(middle(icon), palette.color(QPalette::WindowText));
        EXPECT_EQ(middle(icon, QIcon::Disabled), palette.color(QPalette::PlaceholderText));
        EXPECT_EQ(middle(icon, QIcon::Normal, QIcon::On), palette.color(QPalette::Accent));
        // Outside the shape it stays transparent.
        const QImage image = icon.pixmap(QSize(24, 24), 1.0).toImage();
        EXPECT_EQ(image.pixelColor(1, 1).alpha(), 0);
    }
    // Two themes give two colours: the icon is drawn anew, not cached.
    EXPECT_NE(theme_palette(Qt::ColorScheme::Light).color(QPalette::WindowText),
              theme_palette(Qt::ColorScheme::Dark).color(QPalette::WindowText));
}

TEST_F(ShellIcons, NotAnSvgIsNoIcon) {
    EXPECT_TRUE(fem::shell::tinted_svg_icon("not an svg").isNull());
    EXPECT_TRUE(fem::shell::tinted_svg_icon_file(QStringLiteral("no/such/file.svg")).isNull());
}

TEST_F(ShellIcons, TheDefaultIconIsTheHotTileOfTheSymbol) {
    const QByteArray svg = fem::shell::default_module_icon_svg();
    ASSERT_FALSE(svg.isEmpty());
    QSvgRenderer renderer(svg);
    ASSERT_TRUE(renderer.isValid());
    // The path is the one of the kit file, as it is.
    QFile kit(fem::shell::brand::symbol_small_file);
    ASSERT_TRUE(kit.open(QIODevice::ReadOnly));
    const QString symbol = QString::fromUtf8(kit.readAll());
    const qsizetype start = svg.indexOf("d=\"") + 3;
    const QString path = QString::fromUtf8(svg.mid(start, svg.indexOf('"', start) - start));
    EXPECT_TRUE(symbol.contains(QStringLiteral("fill=\"#FF5A36\" d=\"") + path + '"'));
    // Framed square to the tile.
    EXPECT_EQ(renderer.viewBoxF().width(), renderer.viewBoxF().height());

    const QIcon icon = fem::shell::default_module_icon();
    ASSERT_FALSE(icon.isNull());
    // The outer corner of the tile is solid, in the colour of the text.
    const QImage image = icon.pixmap(QSize(64, 64), 1.0).toImage();
    EXPECT_EQ(image.pixelColor(44, 20),
              theme_palette(Qt::ColorScheme::Light).color(QPalette::WindowText));
    // The hole of the plate is at the opposite corner.
    EXPECT_EQ(image.pixelColor(12, 52).alpha(), 0);
}

TEST_F(ShellIcons, AModuleNamesItsIconByPathOrById) {
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    ASSERT_TRUE(QDir(folder.path()).mkpath(QStringLiteral("icons")));
    QFile file(QDir(folder.path()).filePath(QStringLiteral("icons/square.svg")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(square);
    file.close();
    QFile broken(QDir(folder.path()).filePath(QStringLiteral("icons/broken.svg")));
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
    broken.write("<svg");
    broken.close();

    fem::shell::testing::Application app;
    app.add("test.icons", R"(
[[contributes.icons]]
id = "icons.square"
path = "icons/square.svg"
)",
            std::filesystem::path(folder.path().toStdU16String()));
    const auto icon = [&](std::string_view name, bool fallback) {
        return fem::shell::module_icon(app.manager, "test.icons", name, fallback);
    };
    const QColor text = theme_palette(Qt::ColorScheme::Light).color(QPalette::WindowText);
    EXPECT_EQ(middle(icon("icons/square.svg", false)), text);
    EXPECT_EQ(middle(icon("icons.square", false)), text);
    // Missing or broken: the default icon, or none.
    EXPECT_TRUE(icon("icons/missing.svg", false).isNull());
    EXPECT_TRUE(icon("icons/broken.svg", false).isNull());
    EXPECT_FALSE(icon("icons/broken.svg", true).isNull());
    EXPECT_FALSE(icon("", true).isNull());
    EXPECT_TRUE(icon("", false).isNull());
}

TEST_F(ShellIcons, ColoursOfModesAndStates) {
    const QPalette palette = theme_palette(Qt::ColorScheme::Dark);
    EXPECT_EQ(icon_color(palette, QIcon::Normal, QIcon::Off), palette.color(QPalette::WindowText));
    EXPECT_EQ(icon_color(palette, QIcon::Active, QIcon::Off), palette.color(QPalette::WindowText));
    EXPECT_EQ(icon_color(palette, QIcon::Disabled, QIcon::On),
              palette.color(QPalette::PlaceholderText));
    EXPECT_EQ(icon_color(palette, QIcon::Selected, QIcon::Off),
              palette.color(QPalette::HighlightedText));
    EXPECT_EQ(icon_color(palette, QIcon::Normal, QIcon::On), palette.color(QPalette::Accent));
}

}  // namespace
