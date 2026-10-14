// Pictures of the window for reviews (tasks 1.11-1.13): with STRESSLAB_SCREENSHOT_DIR set, the
// window with a module, a model tree and the form of a material is drawn in the light and dark
// themes, in Russian and English, and saved as PNG there. CI saves
// them as artifacts on each OS; without the variable the test is skipped. Run it on the platform
// of the OS (QT_QPA_PLATFORM=windows or cocoa): the platform without a screen has no fonts on
// Windows.

#include "fem/core/command/model_commands.h"
#include "fem/core/l10n/localization.h"
#include "fem/shell/main_window.h"
#include "fem/shell/model_tree.h"
#include "fem/shell/property_panel.h"
#include "fem/shell/strings.h"
#include "fem/shell/theme.h"
#include "fem/shell/workspaces.h"

#include "shell_test_support.h"
#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QSysInfo>
#include <QToolButton>
#include <QtGlobal>
#include <string>
#include <utility>

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
[[contributes.treeNodeTypes]]
type = "materials"
title = "Материалы"
[[contributes.treeNodeTypes]]
type = "material"
title = "Материал"
[[contributes.treeNodeTypes]]
type = "part"
title = "Деталь"
[[contributes.propertySchemas]]
type = "material"
schema = { type = "object", required = ["E", "nu"], properties = { E = { type = "number", title = "Модуль упругости", x-quantity = "pressure", exclusiveMinimum = 0, x-order = 1 }, nu = { type = "number", title = "Коэффициент Пуассона", minimum = 0, exclusiveMaximum = 0.5, x-order = 2 }, density = { type = "number", title = "Плотность", x-quantity = "density", minimum = 0, x-order = 3 }, model = { type = "string", title = "Модель", enum = ["linear", "plastic"], x-enumTitles = ["Линейно-упругая", "Упругопластическая"], default = "linear", x-order = 4 }, reference = { type = "number", title = "Температура отсчёта", x-quantity = "temperature", x-advanced = true }, note = { type = "string", title = "Примечание", x-advanced = true } } }
)");

/// A model in mm-t-s: a part and two materials, the second without its modulus. Returns both.
std::pair<fem::model::ObjectId, fem::model::ObjectId> build_model(fem::command::CommandBus& bus) {
    namespace mc = fem::command::model_commands;
    using fem::command::Value;
    const auto create = [&bus](std::string type, std::string name, Value::Dict properties,
                               fem::model::ObjectId parent) {
        Value::Dict args{{"type", std::move(type)},
                         {"owner", "demo.optimize"},
                         {"name", std::move(name)},
                         {"properties", std::move(properties)}};
        if (parent.is_valid()) {
            args.emplace("parent", Value(parent));
        }
        return fem::model::ObjectId(
            static_cast<std::uint64_t>(bus.execute(mc::create, args).value.as_integer()));
    };
    bus.execute(mc::set_units,
                {{"length", "mm"}, {"mass", "t"}, {"time", "s"}, {"temperature", "degC"}});
    create("part", fem::shell::testing::utf8(u8"Кронштейн"), {}, {});
    const fem::model::ObjectId materials = create("materials", "", {}, {});
    const fem::model::ObjectId steel =
        create("material", fem::shell::testing::utf8(u8"Сталь 20"),
               {{"E", 210000.0}, {"nu", 0.3}, {"density", 7.85e-9}}, materials);
    const fem::model::ObjectId aluminium =
        create("material", fem::shell::testing::utf8(u8"Алюминий"), {{"nu", 0.33}}, materials);
    return {steel, aluminium};
}

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
        /// The material without its modulus selected, "Advanced" open: a required field empty.
        bool incomplete = false;
    };
    for (const Shot shot :
         {Shot{"ru", Qt::ColorScheme::Light, "light"}, Shot{"ru", Qt::ColorScheme::Dark, "dark"},
          Shot{"en", Qt::ColorScheme::Light, "light"},
          Shot{"ru", Qt::ColorScheme::Light, "light-required", true}}) {
        fem::shell::testing::Application app;
        app.localization.set_language(shot.language);
        fem::shell::apply_theme(*qApp, shot.scheme);
        // A module that adds a stage, a menu, commands of the context toolbar, an activity with
        // the default icon and a tab.
        app.add("demo.optimize", demo_module);
        const auto [steel, aluminium] = build_model(app.bus);
        fem::shell::MainWindow window(app.workbench());
        app.keys.set("stage", "solve");
        window.model_tree().select(shot.incomplete ? aluminium : steel);
        if (shot.incomplete) {
            window.properties().form().advanced_button()->setChecked(true);
        }
        window.show();
        QApplication::processEvents();
        const QString file = QDir(folder).filePath(
            QStringLiteral("stresslab-%1-%2-%3.png").arg(os, shot.theme, shot.language));
        EXPECT_TRUE(window.grab().save(file)) << file.toStdString();
    }
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
}

}  // namespace
