// Pictures of the window for reviews (tasks 1.11-1.15): with STRESSLAB_SCREENSHOT_DIR set, the
// window with a module, a model tree and the form of a material, and the window with the module
// manager, the console and the bar of the modules the model needs, and the window with the module
// fem.demo, enabled and disabled, and the history of a project (task 2.6) are drawn in the light
// and dark themes, in Russian and English, and saved as PNG there. CI saves them as artifacts on
// each OS; without the variable the test is skipped. Run it on the platform of the OS
// (QT_QPA_PLATFORM=windows or cocoa): the platform without a screen has no fonts on Windows.

#include "fem/core/command/journal.h"
#include "fem/core/command/model_commands.h"
#include "fem/core/extension/profile.h"
#include "fem/core/l10n/localization.h"
#include "fem/shell/extensions_panel.h"
#include "fem/shell/file_dialogs.h"
#include "fem/shell/history_controller.h"
#include "fem/shell/history_panel.h"
#include "fem/shell/main_window.h"
#include "fem/shell/model_tree.h"
#include "fem/shell/property_panel.h"
#include "fem/shell/python_console.h"
#include "fem/shell/strings.h"
#include "fem/shell/theme.h"
#include "fem/shell/workspaces.h"
#include "fem/storage/history.h"
#include "fem/storage/session.h"

#include "shell_test_support.h"
#include <DockManager.h>
#include <DockWidget.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QSysInfo>
#include <QToolButton>
#include <QtGlobal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <tuple>
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

/// A console that echoes what it is given, for the pictures.
class EchoConsole final : public fem::shell::ConsoleEngine {
public:
    [[nodiscard]] QString banner() const override {
        return QStringLiteral("Python 3.12, stresslab 0.1.0");
    }
    Reply push(const QString& line) override {
        if (line.startsWith(QLatin1String("print("))) {
            return {.output = {{false, line.mid(7, line.size() - 9) + QLatin1Char('\n')}}};
        }
        return {};
    }
    void reset() override {}
    [[nodiscard]] QStringList complete(const QString&) override { return {}; }
};

// Task 1.14: the module manager with the card of a module, the console, and the bar that names the
// modules the model needs.
TEST(ShellScreenshots, OfTheModuleManagerAndTheConsole) {
    const QString folder = qEnvironmentVariable("STRESSLAB_SCREENSHOT_DIR");
    if (folder.isEmpty()) {
        GTEST_SKIP() << "STRESSLAB_SCREENSHOT_DIR is not set";
    }
    ASSERT_TRUE(QDir().mkpath(folder));
    const QString os = QSysInfo::productType();
    // The card in the light and dark themes, the list of modules with a profile in English.
    for (const auto& [language, scheme, theme, card] :
         {std::tuple{"ru", Qt::ColorScheme::Light, "light", true},
          std::tuple{"en", Qt::ColorScheme::Dark, "dark", true},
          std::tuple{"en", Qt::ColorScheme::Light, "list", false}}) {
        fem::shell::testing::Application app;
        app.localization.set_language(language);
        fem::shell::apply_theme(*qApp, scheme);
        app.add("demo.optimize", demo_module);
        app.add("demo.dynamics", fem::shell::testing::utf8(u8R"(
[[contributes.treeNodeTypes]]
type = "damping"
title = "Демпфирование"
)"));
        fem::command::SessionJournal journal(app.bus);
        (void)build_model(app.bus);
        for (const char* name : {"d1", "d2", "d3"}) {
            (void)app.bus.execute(
                "model.create", {{"type", "damping"}, {"owner", "demo.dynamics"}, {"name", name}});
        }
        (void)app.manager.disable("demo.dynamics");
        EchoConsole console;
        fem::shell::Workbench workbench = app.workbench();
        workbench.console = &console;
        workbench.journal = &journal;
        const QString profiles = QDir(folder).filePath(QStringLiteral("profiles-%1").arg(theme));
        fem::extension::ProfileLibrary library(fem::shell::to_path(profiles));
        workbench.profiles = &library;
        fem::shell::MainWindow window(workbench);
        window.show_extensions();
        window.extensions().save_profile(fem::shell::testing::utf8(u8"Прочность"));
        if (card) {
            window.extensions().show_card("demo.optimize");
        }
        window.console().run(QStringLiteral("print('Привет')"));
        window.console().run(QStringLiteral("stresslab.model.rename(object=1, name='Опора')"));
        window.check_required_modules();
        if (scheme == Qt::ColorScheme::Dark) {
            // The journal is in front otherwise.
            qobject_cast<ads::CDockWidget*>(window.zone_widget(fem::shell::zone::python_console))
                ->setAsCurrentTab();
        }
        window.show();
        QApplication::processEvents();
        const QString file = QDir(folder).filePath(
            QStringLiteral("stresslab-%1-modules-%2-%3.png").arg(os, theme, language));
        EXPECT_TRUE(window.grab().save(file)) << file.toStdString();
        QDir(profiles).removeRecursively();
    }
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
}

// Task 1.15: the module fem.demo the application ships, on its stage, with a note selected and
// its tab in front; and the window after it is disabled from the console.
TEST(ShellScreenshots, OfTheDemoModule) {
    const QString folder = qEnvironmentVariable("STRESSLAB_SCREENSHOT_DIR");
    if (folder.isEmpty()) {
        GTEST_SKIP() << "STRESSLAB_SCREENSHOT_DIR is not set";
    }
    ASSERT_TRUE(QDir().mkpath(folder));
    const QString os = QSysInfo::productType();
    for (const auto& [language, scheme, theme, enabled] :
         {std::tuple{"ru", Qt::ColorScheme::Light, "light", true},
          std::tuple{"en", Qt::ColorScheme::Dark, "dark", true},
          std::tuple{"ru", Qt::ColorScheme::Light, "disabled", false}}) {
        fem::shell::testing::Application app;
        app.localization.set_language(language);
        fem::shell::apply_theme(*qApp, scheme);
        app.add_demo();
        fem::command::SessionJournal journal(app.bus);
        const auto note = [&app](const char* name, const std::string& text, const char* priority) {
            return fem::model::ObjectId(static_cast<std::uint64_t>(
                app.bus
                    .execute(fem::command::model_commands::create,
                             {{"type", "demo_note"},
                              {"owner", "fem.demo"},
                              {"name", name},
                              {"properties",
                               fem::command::Value::Dict{{"text", text}, {"priority", priority}}}})
                    .value.as_integer()));
        };
        const fem::model::ObjectId first =
            note("#1", fem::shell::testing::utf8(u8"Проверить опору"), "high");
        (void)note("#2", "TODO", "normal");
        EchoConsole console;
        fem::shell::Workbench workbench = app.workbench();
        workbench.console = &console;
        workbench.journal = &journal;
        fem::shell::MainWindow window(workbench);
        app.keys.set("stage", "demo.review");
        window.model_tree().select(first);
        if (enabled) {
            if (auto* tab =
                    window.dock_manager()->findDockWidget(QStringLiteral("tab:demo.notes"))) {
                tab->setAsCurrentTab();
            }
        } else {
            (void)app.manager.disable("fem.demo", fem::extension::Journaling::on);
            qobject_cast<ads::CDockWidget*>(window.zone_widget(fem::shell::zone::journal))
                ->setAsCurrentTab();
        }
        window.show();
        QApplication::processEvents();
        const QString file = QDir(folder).filePath(
            QStringLiteral("stresslab-%1-demo-%2-%3.png").arg(os, theme, language));
        EXPECT_TRUE(window.grab().save(file)) << file.toStdString();
    }
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
}

}  // namespace

// Task 2.6: the panel "History" with two variants, the commit form and the switcher of variants;
// a version viewed with the bar over the 3D view; the screen "Compare" with the key indicators.
TEST(ShellScreenshots, OfTheHistory) {
    const QString folder = qEnvironmentVariable("STRESSLAB_SCREENSHOT_DIR");
    if (folder.isEmpty()) {
        GTEST_SKIP() << "STRESSLAB_SCREENSHOT_DIR is not set";
    }
    ASSERT_TRUE(QDir().mkpath(folder));
    const QString os = QSysInfo::productType();
    using fem::shell::testing::utf8;
    namespace fs = std::filesystem;

    struct Shot {
        const char* language;
        Qt::ColorScheme scheme;
        const char* theme;
    };
    for (const Shot shot :
         {Shot{"ru", Qt::ColorScheme::Light, "light"}, Shot{"ru", Qt::ColorScheme::Dark, "dark"},
          Shot{"en", Qt::ColorScheme::Light, "light"}}) {
        const fs::path root = fs::temp_directory_path() / fs::path(u8"stresslab снимки истории");
        std::error_code ignored;
        fs::remove_all(root, ignored);
        fs::create_directories(root);
        {
            fem::shell::testing::Application app;
            app.localization.set_language(shot.language);
            fem::shell::apply_theme(*qApp, shot.scheme);
            app.add("demo.optimize", demo_module);
            app.add("demo.results", R"(
[[contributes.resultIndicators]]
id = "demo.mass"
title = "Mass"
quantity = "mass"
[[contributes.resultIndicators]]
id = "demo.stress"
title = "Max stress"
quantity = "pressure"
[[contributes.resultIndicators]]
id = "demo.safety"
title = "Safety factor"
)");
            const auto [steel, aluminium] = build_model(app.bus);
            fem::storage::ProjectSession project(app.bus);
            fem::shell::Workbench workbench = app.workbench();
            workbench.project = &project;
            // No dialog may block the pictures.
            workbench.ask_pending = [](QWidget*, QString&) {
                return fem::shell::PendingChanges::stash;
            };
            fem::shell::MainWindow window(workbench);
            project.save_as(root / fs::path(u8"кронштейн.slproj"));
            fem::storage::ProjectHistory& history = project.history();
            const auto set = [&app](fem::model::ObjectId id, const char* key, double value) {
                (void)app.bus.execute("model.set_property",
                                      {{"object", id}, {"key", key}, {"value", value}});
            };
            const std::string first = history.commit(utf8(u8"Кронштейн из стали 20"));
            history.tag(utf8(u8"Отправлено на согласование"));
            set(aluminium, "E", 70000.0);
            (void)history.snapshot("Before solving");
            set(steel, "density", 7.8e-9);
            const std::string second = history.commit(utf8(u8"Уточнена плотность стали"));
            history.branch(utf8(u8"Композитная обшивка"), first);
            history.checkout(utf8(u8"Композитная обшивка"));
            set(steel, "E", 120000.0);
            const std::string composite = history.commit(utf8(u8"Обшивка из КМ-1, масса −12%"));
            set(steel, "nu", 0.28);
            // Results of two versions, with key indicators.
            fem::storage::ResultCache& results = project.results();
            const fem::storage::SolverInfo solver{"calculix", "2.22"};
            const fs::path out = root / "out";
            fs::create_directories(out);
            std::ofstream(out / "result.frd") << "result";
            (void)results.put(
                results.key_at(second, solver), second, solver, {}, out,
                {{"demo.mass", 0.0412}, {"demo.stress", 182.5}, {"demo.safety", 1.9}});
            (void)results.put(
                results.key_at(composite, solver), composite, solver, {}, out,
                {{"demo.mass", 0.0363}, {"demo.stress", 204.0}, {"demo.safety", 1.7}});
            window.history().refresh();
            window.show_history();
            window.resize(1440, 860);
            (void)window.history_panel().start_commit();
            window.show();
            QApplication::processEvents();
            const auto save = [&](const char* what) {
                QApplication::processEvents();
                const QString file = QDir(folder).filePath(
                    QStringLiteral("stresslab-history-%1-%2-%3-%4.png")
                        .arg(os, QString::fromLatin1(what), shot.theme, shot.language));
                EXPECT_TRUE(window.grab().save(file)) << file.toStdString();
            };
            save("panel");
            window.history_panel().cancel_commit();
            (void)app.bus.undo();
            (void)window.history().view(first);
            save("viewing");
            (void)window.history().return_to_current();
            window.show_comparison(fem::shell::to_qstring(second),
                                   fem::shell::to_qstring(composite));
            save("compare");
        }
        fs::remove_all(root, ignored);
    }
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
}
