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

#include "shell_test_support.h"
#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QSysInfo>
#include <QtGlobal>
#include <string>

namespace {

const std::string demo_module = fem::shell::testing::utf8(
    u8R"(
[[contributes.commands]]
id = "optimize.run"
title = "Оптимизировать"
category = "Оптимизация"
[[contributes.commands]]
id = "optimize.check"
title = "Проверить модель"
[[contributes.commands]]
id = "optimize.report"
title = "Отчёт"
enablement = "model.hasMesh"
[[contributes.stages]]
id = "optimize.stage"
title = "Оптимизация"
slot = "after:results"
[[contributes.submenus]]
id = "optimize"
title = "Оптимизация"
[[contributes.menus]]
location = "optimize"
command = "optimize.run"
[[contributes.menus]]
location = "optimize"
command = "optimize.report"
[[contributes.contextToolbar]]
command = "optimize.check"
group = "1_check"
when = "stage == 'solve'"
[[contributes.contextToolbar]]
command = "optimize.run"
group = "2_run"
when = "stage == 'solve'"
[[contributes.contextToolbar]]
command = "optimize.report"
group = "more"
when = "stage == 'solve'"
[[contributes.activityBar]]
id = "optimize.history"
title = "История оптимизации"
[[contributes.bottomPanelTabs]]
id = "optimize.log"
title = "Лог оптимизации"
[[contributes.keybindings]]
command = "optimize.run"
key = "Mod+Shift+O"
)");

TEST(ShellScreenshots, OfTheWindow) {
    const QString folder = qEnvironmentVariable("STRESSLAB_SCREENSHOT_DIR");
    if (folder.isEmpty()) {
        GTEST_SKIP() << "STRESSLAB_SCREENSHOT_DIR is not set";
    }
    ASSERT_TRUE(QDir().mkpath(folder));
    const QString os = QSysInfo::productType();

    struct Shot {
        const char* language;
        Qt::ColorScheme scheme;
        const char* theme;
    };
    for (const Shot shot :
         {Shot{"ru", Qt::ColorScheme::Light, "light"}, Shot{"ru", Qt::ColorScheme::Dark, "dark"},
          Shot{"en", Qt::ColorScheme::Light, "light"}}) {
        fem::shell::testing::Application app;
        app.localization.set_language(shot.language);
        fem::shell::apply_theme(*qApp, shot.scheme);
        // A module that adds a stage, a menu, commands of the context toolbar, an activity with
        // the default icon and a tab.
        app.add("demo.optimize", demo_module);
        fem::shell::MainWindow window(app.workbench());
        app.keys.set("stage", "solve");
        window.show();
        QApplication::processEvents();
        const QString file = QDir(folder).filePath(
            QStringLiteral("stresslab-%1-%2-%3.png").arg(os, shot.theme, shot.language));
        EXPECT_TRUE(window.grab().save(file)) << file.toStdString();
    }
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
}

}  // namespace
