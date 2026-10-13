// Pictures of the window for reviews (task 1.11): with STRESSLAB_SCREENSHOT_DIR set, the window is
// drawn in the light and dark themes, in Russian and English, and saved as PNG there. CI saves
// them as artifacts on each OS; without the variable the test is skipped. Run it on the platform
// of the OS (QT_QPA_PLATFORM=windows or cocoa): the platform without a screen has no fonts on
// Windows.

#include "fem/core/l10n/localization.h"
#include "fem/shell/main_window.h"
#include "fem/shell/strings.h"
#include "fem/shell/theme.h"
#include "fem/shell/workspaces.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QSysInfo>
#include <QtGlobal>
#include <string>

namespace {

TEST(ShellScreenshots, OfTheWindow) {
    const QString folder = qEnvironmentVariable("STRESSLAB_SCREENSHOT_DIR");
    if (folder.isEmpty()) {
        GTEST_SKIP() << "STRESSLAB_SCREENSHOT_DIR is not set";
    }
    ASSERT_TRUE(QDir().mkpath(folder));
    const QString os = QSysInfo::productType();

    fem::l10n::Localization localization;
    fem::shell::add_shell_catalogs(localization);
    struct Shot {
        const char* language;
        Qt::ColorScheme scheme;
        const char* theme;
    };
    for (const Shot shot :
         {Shot{"ru", Qt::ColorScheme::Light, "light"}, Shot{"ru", Qt::ColorScheme::Dark, "dark"},
          Shot{"en", Qt::ColorScheme::Light, "light"}}) {
        localization.set_language(shot.language);
        fem::shell::apply_theme(*qApp, shot.scheme);
        fem::shell::WorkspaceStore workspaces;
        fem::shell::MainWindow window(localization, workspaces);
        window.show();
        QApplication::processEvents();
        const QString file = QDir(folder).filePath(
            QStringLiteral("stresslab-%1-%2-%3.png").arg(os, shot.theme, shot.language));
        EXPECT_TRUE(window.grab().save(file)) << file.toStdString();
    }
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
}

}  // namespace
