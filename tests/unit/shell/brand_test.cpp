#include "fem/shell/brand.h"

#include "fem/core/l10n/localization.h"
#include "fem/core/product.h"
#include "fem/shell/about_dialog.h"
#include "fem/shell/strings.h"
#include "fem/shell/theme.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QImage>
#include <QLabel>

namespace {

namespace brand = fem::shell::brand;

QByteArray read(const QString& file) {
    QFile input(file);
    EXPECT_TRUE(input.open(QIODevice::ReadOnly)) << file.toStdString();
    return input.readAll();
}

QImage image_of(const QPixmap& pixmap) {
    return pixmap.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage rendered(const QString& file, int device_pixels) {
    return image_of(brand::render_svg(file, QSize(device_pixels, device_pixels)));
}

TEST(ShellBrand, TheKitFilesAreBuiltInAsTheyAre) {
    // The resources are the files of the kit by the paths of cmake/Branding.cmake, not copies.
    EXPECT_EQ(read(brand::symbol_small_file), read(QString::fromUtf8(FEM_BRANDING_SYMBOL_SMALL)));
    EXPECT_EQ(read(brand::app_icon_file), read(QString::fromUtf8(FEM_BRANDING_ICON_SVG)));
    EXPECT_EQ(read(brand::logo_color_file), read(QString::fromUtf8(FEM_BRANDING_LOGO_COLOR)));
    EXPECT_EQ(read(brand::logo_reversed_file), read(QString::fromUtf8(FEM_BRANDING_LOGO_REVERSED)));
}

TEST(ShellBrand, DrawsForTheDevicePixelRatio) {
    const QPixmap pixmap = brand::render_svg(brand::symbol_small_file, QSize(16, 16), 2.0);
    ASSERT_FALSE(pixmap.isNull());
    EXPECT_EQ(pixmap.size(), QSize(32, 32));
    EXPECT_DOUBLE_EQ(pixmap.devicePixelRatio(), 2.0);
    EXPECT_EQ(pixmap.deviceIndependentSize(), QSizeF(16, 16));
    // Something is drawn: the symbol is not transparent all over.
    const QImage image = image_of(pixmap);
    bool opaque = false;
    for (int y = 0; y < image.height() && !opaque; ++y) {
        for (int x = 0; x < image.width() && !opaque; ++x) {
            opaque = qAlpha(image.pixel(x, y)) == 255;
        }
    }
    EXPECT_TRUE(opaque);
    EXPECT_TRUE(brand::render_svg(QStringLiteral(":/no/such.svg"), QSize(16, 16)).isNull());
}

TEST(ShellBrand, SmallIconsAreTheSmallSizeSymbol) {
    const QIcon icon = brand::application_icon();
    for (const int size : {16, 20, 24, 32}) {
        EXPECT_EQ(image_of(icon.pixmap(QSize(size, size))),
                  rendered(brand::symbol_small_file, size))
            << size;
    }
    for (const int size : {48, 64, 256}) {
        EXPECT_EQ(image_of(icon.pixmap(QSize(size, size))), rendered(brand::app_icon_file, size))
            << size;
    }
}

TEST(ShellBrand, IconsAreSharpOnHighDensityScreens) {
    const QIcon icon = brand::application_icon();
    // 16 logical pixels at 2x are 32 device pixels: still the small-size symbol.
    const QPixmap small = icon.pixmap(QSize(16, 16), 2.0);
    EXPECT_EQ(small.size(), QSize(32, 32));
    EXPECT_EQ(image_of(small), rendered(brand::symbol_small_file, 32));
    // 32 at 2x are 64: the application icon.
    const QPixmap large = icon.pixmap(QSize(32, 32), 2.0);
    EXPECT_EQ(large.size(), QSize(64, 64));
    EXPECT_EQ(image_of(large), rendered(brand::app_icon_file, 64));
}

TEST(ShellBrand, TheLogoFollowsTheTheme) {
    EXPECT_EQ(brand::logo_file(fem::shell::theme_palette(Qt::ColorScheme::Light)),
              brand::logo_color_file);
    EXPECT_EQ(brand::logo_file(fem::shell::theme_palette(Qt::ColorScheme::Dark)),
              brand::logo_reversed_file);
}

TEST(ShellAbout, ShowsTheLogoOfTheThemeAndTheProduct) {
    fem::l10n::Localization localization;
    fem::shell::add_shell_catalogs(localization);
    fem::shell::AboutDialog dialog(localization);
    dialog.setPalette(fem::shell::theme_palette(Qt::ColorScheme::Light));
    EXPECT_EQ(dialog.logo_file(), brand::logo_color_file);
    EXPECT_EQ(dialog.windowTitle(), "About " + fem::shell::to_qstring(fem::kProductName));

    const auto* logo = dialog.findChild<QLabel*>(QStringLiteral("aboutLogo"));
    ASSERT_NE(logo, nullptr);
    // Not smaller than the kit allows (guidelines §3), proportions kept.
    EXPECT_GE(logo->pixmap().deviceIndependentSize().width(), 96.0);
    EXPECT_GT(logo->pixmap().deviceIndependentSize().width(),
              logo->pixmap().deviceIndependentSize().height());

    dialog.setPalette(fem::shell::theme_palette(Qt::ColorScheme::Dark));
    EXPECT_EQ(dialog.logo_file(), brand::logo_reversed_file);
    EXPECT_EQ(image_of(logo->pixmap()).size(),
              image_of(brand::render_svg(brand::logo_reversed_file,
                                         logo->pixmap().deviceIndependentSize().toSize(),
                                         logo->pixmap().devicePixelRatio()))
                  .size());

    localization.set_language("ru");
    fem::shell::AboutDialog russian(localization);
    EXPECT_EQ(russian.windowTitle(), "О программе " + fem::shell::to_qstring(fem::kProductName));
}

}  // namespace
