// The window built from the registry of contributions (task 1.12): items of modules appear when a
// module is enabled and disappear when it is disabled, and show or hide as context keys change,
// without a restart.

#include "fem/shell/contribution_ui.h"

#include "fem/core/context/value.h"
#include "fem/shell/keys.h"
#include "fem/shell/main_window.h"
#include "fem/shell/theme.h"

#include "shell_test_support.h"
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace zone = fem::shell::zone;
using fem::context::ContextValue;
using fem::shell::MainWindow;
using fem::shell::testing::find_action;
using fem::shell::testing::q;
using fem::shell::testing::shown_items;
using fem::shell::testing::utf8;

/// A module with something at every place of the window.
constexpr std::string_view everywhere = R"(
[[contributes.commands]]
id = "opt.run"
title = "Run Optimization"
[[contributes.commands]]
id = "opt.stop"
title = "Stop Optimization"
[[contributes.submenus]]
id = "optimize"
title = "Optimization"
[[contributes.menus]]
location = "optimize"
command = "opt.run"
[[contributes.menus]]
location = "optimize/advanced"
command = "opt.stop"
[[contributes.menus]]
location = "file"
command = "opt.stop"
[[contributes.stages]]
id = "opt.stage"
title = "Optimization"
slot = "after:results"
[[contributes.contextToolbar]]
command = "opt.run"
[[contributes.contextMenus]]
location = "tree"
command = "opt.stop"
[[contributes.activityBar]]
id = "opt.history"
title = "Iterations"
[[contributes.bottomPanelTabs]]
id = "opt.log"
title = "Optimization Log"
[[contributes.keybindings]]
command = "opt.run"
key = "Mod+Shift+O"
)";

class ShellContributions : public ::testing::Test {
protected:
    void SetUp() override {
        fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
        window_ = std::make_unique<MainWindow>(app_.workbench());
        window_->show();
        QApplication::processEvents();
    }
    void TearDown() override { window_.reset(); }

    [[nodiscard]] QMenu* menu(const QString& name) const {
        return window_->menuBar()->findChild<QMenu*>(name);
    }
    [[nodiscard]] QToolBar* bar(std::string_view id) const {
        return qobject_cast<QToolBar*>(window_->zone_widget(id));
    }
    [[nodiscard]] ads::CDockWidget* panel(const QString& name) const {
        return window_->dock_manager()->findDockWidget(name);
    }
    [[nodiscard]] QStringList stages() const {
        QStringList ids;
        for (const QAction* action : bar(zone::stage_bar)->actions()) {
            if (action->isVisible()) {
                ids.push_back(action->objectName().mid(6));  // Without "stage.".
            }
        }
        return ids;
    }
    /// The commands the context toolbar shows.
    [[nodiscard]] QStringList toolbar() const {
        QStringList ids;
        for (const QAction* action : window_->context_toolbar()->actions()) {
            if (action->isVisible() && action->objectName().contains(QLatin1Char('.')) &&
                !action->objectName().startsWith(QStringLiteral("contextToolbar"))) {
                ids.push_back(action->objectName());
            }
        }
        return ids;
    }
    /// The commands of the "⋯" menu that show.
    [[nodiscard]] QStringList overflow() const {
        QStringList ids;
        for (const QAction* action : window_->contributions().overflow_menu()->actions()) {
            if (action->isVisible()) {
                ids.push_back(action->objectName());
            }
        }
        return ids;
    }
    [[nodiscard]] QMenuBar* menu_bar() const { return window_->menuBar(); }
    [[nodiscard]] QStringList menu_bar_titles() const {
        QStringList titles;
        for (const QAction* action : menu_bar()->actions()) {
            if (action->isVisible()) {
                titles.push_back(action->menu()->objectName());
            }
        }
        return titles;
    }
    void set(const std::string& key, ContextValue value) { app_.keys.set(key, std::move(value)); }

    fem::shell::testing::Application app_;
    std::unique_ptr<MainWindow> window_;
};

TEST_F(ShellContributions, EnablingAModuleAddsItsItemsAndDisablingRemovesThem) {
    const QString empty_file_menu = shown_items(menu(QStringLiteral("menu.file"))).join('|');
    std::vector<bool> zones_before;
    for (const std::string_view id : {zone::model_tree, zone::properties, zone::journal}) {
        zones_before.push_back(window_->is_zone_shown(id));
    }
    ads::CDockAreaWidget* bottom =
        window_->dock_manager()->findDockWidget(q(zone::journal))->dockAreaWidget();

    app_.add("test.opt", everywhere);

    // Its menu, before Help, with its title and a submenu.
    ASSERT_NE(menu(QStringLiteral("menu.optimize")), nullptr);
    EXPECT_EQ(menu(QStringLiteral("menu.optimize"))->title(), QStringLiteral("Optimization"));
    EXPECT_EQ(menu_bar_titles(),
              QStringList({"menu.file", "menu.edit", "menu.view", "menu.optimize", "menu.help"}));
    EXPECT_EQ(shown_items(menu(QStringLiteral("menu.optimize"))),
              QStringList({"Run Optimization", "---", "advanced"}));
    ASSERT_NE(menu(QStringLiteral("menu.optimize/advanced")), nullptr);
    EXPECT_EQ(shown_items(menu(QStringLiteral("menu.optimize/advanced"))),
              QStringList({"Stop Optimization"}));
    // An item in a menu of the shell goes after its own ones.
    EXPECT_EQ(shown_items(menu(QStringLiteral("menu.file"))).join('|'),
              empty_file_menu + QStringLiteral("|---|Stop Optimization"));
    // Its stage after "Results", its command on the context toolbar.
    EXPECT_EQ(stages(), QStringList({"geometry", "mesh", "materials", "steps", "solve", "results",
                                     "opt.stage"}));
    EXPECT_EQ(toolbar(), QStringList({"opt.run"}));
    EXPECT_TRUE(window_->context_toolbar()->isVisibleTo(window_.get()));
    // Its activity, closed until clicked, and its tab next to the journal.
    QAction* activity =
        find_action(bar(zone::activity_bar), QStringLiteral("activity.opt.history"));
    ASSERT_NE(activity, nullptr);
    EXPECT_FALSE(activity->icon().isNull());
    ASSERT_NE(panel(QStringLiteral("activity:opt.history")), nullptr);
    EXPECT_TRUE(panel(QStringLiteral("activity:opt.history"))->isClosed());
    activity->trigger();
    EXPECT_FALSE(panel(QStringLiteral("activity:opt.history"))->isClosed());
    ASSERT_NE(panel(QStringLiteral("tab:opt.log")), nullptr);
    EXPECT_FALSE(panel(QStringLiteral("tab:opt.log"))->isClosed());
    EXPECT_EQ(panel(QStringLiteral("tab:opt.log"))->dockAreaWidget(), bottom);
    // The journal stays in front.
    EXPECT_EQ(bottom->currentDockWidget(), panel(q(zone::journal)));
    EXPECT_EQ(panel(QStringLiteral("tab:opt.log"))->windowTitle(),
              QStringLiteral("Optimization Log"));
    // Its shortcut.
    EXPECT_NE(find_action(window_.get(), QStringLiteral("key.Ctrl+Shift+O")), nullptr);

    (void)app_.manager.disable("test.opt");

    EXPECT_EQ(menu(QStringLiteral("menu.optimize")), nullptr);
    EXPECT_EQ(menu(QStringLiteral("menu.optimize/advanced")), nullptr);
    EXPECT_EQ(shown_items(menu(QStringLiteral("menu.file"))).join('|'), empty_file_menu);
    EXPECT_EQ(stages(),
              QStringList({"geometry", "mesh", "materials", "steps", "solve", "results"}));
    EXPECT_TRUE(toolbar().isEmpty());
    EXPECT_FALSE(window_->context_toolbar()->isVisibleTo(window_.get()));
    EXPECT_TRUE(bar(zone::activity_bar)->actions().isEmpty());
    EXPECT_EQ(panel(QStringLiteral("activity:opt.history")), nullptr);
    EXPECT_EQ(panel(QStringLiteral("tab:opt.log")), nullptr);
    EXPECT_EQ(find_action(window_.get(), QStringLiteral("key.Ctrl+Shift+O")), nullptr);
    EXPECT_TRUE(window_->contributions().module_panels().empty());
    // The zones of the shell are where they were.
    std::vector<bool> zones_after;
    for (const std::string_view id : {zone::model_tree, zone::properties, zone::journal}) {
        zones_after.push_back(window_->is_zone_shown(id));
    }
    EXPECT_EQ(zones_after, zones_before);

    // And back again, without a restart.
    (void)app_.manager.enable("test.opt");
    EXPECT_NE(menu(QStringLiteral("menu.optimize")), nullptr);
    EXPECT_EQ(toolbar(), QStringList({"opt.run"}));
}

TEST_F(ShellContributions, ItemsFollowTheirWhenClauses) {
    app_.add("test.cond", R"(
[[contributes.commands]]
id = "cond.run"
title = "Run"
[[contributes.menus]]
location = "solve"
command = "cond.run"
when = "cond.ready"
[[contributes.contextToolbar]]
command = "cond.run"
when = "cond.ready && stage == 'solve'"
[[contributes.stages]]
id = "cond.stage"
title = "Check"
slot = "before:solve"
when = "cond.ready"
[[contributes.bottomPanelTabs]]
id = "cond.monitor"
title = "Monitor"
when = "cond.ready"
[[contributes.contextKeys]]
name = "cond.ready"
type = "boolean"
default = false
)");
    ads::CDockWidget* monitor = panel(QStringLiteral("tab:cond.monitor"));
    ASSERT_NE(monitor, nullptr);
    EXPECT_TRUE(monitor->isClosed());
    EXPECT_FALSE(menu(QStringLiteral("menu.solve"))->menuAction()->isVisible());
    EXPECT_FALSE(stages().contains(QStringLiteral("cond.stage")));

    set("cond.ready", true);
    EXPECT_TRUE(menu(QStringLiteral("menu.solve"))->menuAction()->isVisible());
    EXPECT_EQ(menu(QStringLiteral("menu.solve"))->title(), QStringLiteral("Solve"));
    EXPECT_EQ(stages(), QStringList({"geometry", "mesh", "materials", "steps", "cond.stage",
                                     "solve", "results"}));
    EXPECT_FALSE(monitor->isClosed());
    EXPECT_TRUE(toolbar().isEmpty());
    set("stage", "solve");
    EXPECT_EQ(toolbar(), QStringList({"cond.run"}));
    EXPECT_TRUE(find_action(bar(zone::stage_bar), QStringLiteral("stage.solve"))->isChecked());

    // The current stage hides: back to the first one.
    find_action(bar(zone::stage_bar), QStringLiteral("stage.cond.stage"))->trigger();
    EXPECT_EQ(app_.keys.value("stage"), ContextValue("cond.stage"));
    set("cond.ready", false);
    EXPECT_EQ(app_.keys.value("stage"), ContextValue("geometry"));
    EXPECT_TRUE(find_action(bar(zone::stage_bar), QStringLiteral("stage.geometry"))->isChecked());
    EXPECT_FALSE(menu(QStringLiteral("menu.solve"))->menuAction()->isVisible());
    EXPECT_TRUE(monitor->isClosed());
    EXPECT_TRUE(toolbar().isEmpty());
}

TEST_F(ShellContributions, UnavailableCommandsAreGreyWithTheReason) {
    app_.add("test.grey", R"(
[[contributes.commands]]
id = "grey.report"
title = "Report"
enablement = "model.hasMesh && grey.ready"
[[contributes.commands]]
id = "grey.export"
title = "Export"
args = [{ name = "file", type = "path" }]
[[contributes.menus]]
location = "results"
command = "grey.report"
[[contributes.menus]]
location = "results"
command = "grey.export"
[[contributes.commands]]
id = "grey.check"
title = "Check"
enablement = "grey.ready && stage == 'results'"
[[contributes.menus]]
location = "results"
command = "grey.check"
[[contributes.contextKeys]]
name = "grey.ready"
type = "boolean"
default = true
)");
    QMenu* results = menu(QStringLiteral("menu.results"));
    QAction* report = find_action(results, QStringLiteral("grey.report"));
    QAction* export_ = find_action(results, QStringLiteral("grey.export"));
    QAction* check = find_action(results, QStringLiteral("grey.check"));
    ASSERT_NE(report, nullptr);
    ASSERT_NE(export_, nullptr);
    ASSERT_NE(check, nullptr);
    EXPECT_TRUE(report->isVisible());
    EXPECT_FALSE(report->isEnabled());
    EXPECT_EQ(report->toolTip(), QStringLiteral("Requires a mesh in the model"));
    EXPECT_FALSE(export_->isEnabled());
    EXPECT_EQ(export_->toolTip(), QStringLiteral("Needs arguments: file"));
    EXPECT_FALSE(check->isEnabled());
    EXPECT_EQ(check->toolTip(), QStringLiteral("Requires the stage “Results”"));

    // The value of the condition stays false; the reason is fresh when the menu opens.
    set("grey.ready", false);
    Q_EMIT results->aboutToShow();
    EXPECT_EQ(report->toolTip(), QStringLiteral("Requires a mesh in the model, grey.ready"));

    // Available at once when its condition holds.
    set("grey.ready", true);
    set("stage", "results");
    EXPECT_TRUE(check->isEnabled());
    EXPECT_EQ(check->toolTip(), QStringLiteral("Check"));
    check->trigger();
    EXPECT_EQ(app_.runtime->log, std::vector<std::string>{"execute grey.check"});
    // A grey item does nothing.
    report->trigger();
    EXPECT_EQ(app_.runtime->log.size(), 1U);
}

TEST_F(ShellContributions, TheContextToolbarShowsTwelveCommandsAndTheRestUnderMore) {
    std::string points;
    for (int i = 0; i < 15; ++i) {
        const std::string id = "many.c" + std::string(i < 10 ? "0" : "") + std::to_string(i);
        points += "[[contributes.commands]]\nid = \"" + id + "\"\ntitle = \"C" + std::to_string(i) +
                  "\"\n[[contributes.contextToolbar]]\ncommand = \"" + id + "\"\ngroup = \"" +
                  (i == 0       ? "more"
                   : i % 2 == 0 ? "2_even"
                                : "1_odd") +
                  "@" + std::to_string(100 - i) + "\"\n";
    }
    app_.add("test.many", points);
    const QStringList shown = toolbar();
    ASSERT_EQ(shown.size(), fem::shell::ContributionUi::max_toolbar_commands);
    // Group "1_odd" first, inside a group by order (larger i, smaller order, first).
    EXPECT_EQ(shown.mid(0, 3), QStringList({"many.c13", "many.c11", "many.c09"}));
    EXPECT_EQ(shown.back(), QStringLiteral("many.c06"));
    // The rest, and the rare command, under "⋯".
    EXPECT_EQ(overflow(), QStringList({"many.c04", "many.c02", "many.c00"}));
    EXPECT_TRUE(window_->contributions().overflow_shown());
}

TEST_F(ShellContributions, StagesGoOnlyIntoTheSlotsOfStandardStages) {
    app_.add("test.stages", R"(
[[contributes.stages]]
id = "st.first"
title = "First"
slot = "before:mesh"
[[contributes.stages]]
id = "st.second"
title = "Second"
slot = "before:mesh"
[[contributes.stages]]
id = "st.after"
title = "After"
slot = "after:mesh"
[[contributes.stages]]
id = "st.lost"
title = "Lost"
slot = "after:somewhere"
)");
    EXPECT_EQ(stages(), QStringList({"geometry", "st.first", "st.second", "mesh", "st.after",
                                     "materials", "steps", "solve", "results"}));
    QAction* after = find_action(bar(zone::stage_bar), QStringLiteral("stage.st.after"));
    after->trigger();
    EXPECT_EQ(app_.keys.value("stage"), ContextValue("st.after"));
    EXPECT_TRUE(after->isChecked());
    EXPECT_FALSE(find_action(bar(zone::stage_bar), QStringLiteral("stage.geometry"))->isChecked());
}

TEST_F(ShellContributions, ShortcutsRunTheCommandWhoseConditionHolds) {
    app_.add("test.keys", R"(
[[contributes.commands]]
id = "keys.solve"
title = "Solve"
[[contributes.commands]]
id = "keys.mesh"
title = "Mesh"
[[contributes.commands]]
id = "keys.palette"
title = "Steals the palette"
[[contributes.menus]]
location = "tools"
command = "keys.solve"
[[contributes.keybindings]]
command = "keys.solve"
key = "Mod+Shift+O"
when = "stage == 'solve'"
[[contributes.keybindings]]
command = "keys.mesh"
key = "Mod+Shift+O"
when = "stage == 'mesh'"
[[contributes.keybindings]]
command = "keys.palette"
key = "Mod+Shift+P"
)");
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    QAction* key = find_action(window_.get(), QStringLiteral("key.Ctrl+Shift+O"));
    ASSERT_NE(key, nullptr);
    // At the first stage neither binding holds.
    EXPECT_FALSE(key->isEnabled());
    QTest::keyClick(window_.get(), Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_TRUE(app_.runtime->log.empty());

    set("stage", "solve");
    EXPECT_TRUE(key->isEnabled());
    QTest::keyClick(window_.get(), Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
    set("stage", "mesh");
    QTest::keyClick(window_.get(), Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(app_.runtime->log,
              (std::vector<std::string>{"execute keys.solve", "execute keys.mesh"}));

    // The shortcut of the palette stays the shell's; the menu item shows the key of its command.
    EXPECT_EQ(find_action(window_.get(), QStringLiteral("key.Ctrl+Shift+P")), nullptr);
    QAction* item = find_action(menu(QStringLiteral("menu.tools")), QStringLiteral("keys.solve"));
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->shortcut(), fem::shell::key_sequence("Mod+Shift+O"));
    EXPECT_EQ(item->shortcutContext(), Qt::WidgetShortcut);
}

TEST_F(ShellContributions, ContextMenusShowWhatHoldsThere) {
    app_.add("test.ctx", R"(
[[contributes.commands]]
id = "ctx.solve"
title = "Solve Here"
[[contributes.commands]]
id = "ctx.always"
title = "Always"
[[contributes.contextMenus]]
location = "tree"
command = "ctx.solve"
when = "stage == 'solve'"
group = "1_main"
[[contributes.contextMenus]]
location = "tree"
command = "ctx.always"
group = "2_other"
[[contributes.contextMenus]]
location = "viewport"
command = "ctx.solve"
when = "stage == 'solve'"
)");
    EXPECT_EQ(window_->contributions().context_menu("viewport"), nullptr);
    std::unique_ptr<QMenu> tree(window_->contributions().context_menu("tree"));
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(shown_items(tree.get()), QStringList({"Always"}));

    set("stage", "solve");
    tree.reset(window_->contributions().context_menu("tree"));
    EXPECT_EQ(shown_items(tree.get()), QStringList({"Solve Here", "---", "Always"}));
    tree->actions().front()->trigger();
    EXPECT_EQ(app_.runtime->log, std::vector<std::string>{"execute ctx.solve"});

    // A right click on the model tree opens it.
    QWidget* target =
        qobject_cast<ads::CDockWidget*>(window_->zone_widget(zone::model_tree))->widget();
    ASSERT_NE(target, nullptr);
    QMenu* shown = window_->show_context_menu("tree", target, QPoint(5, 5));
    ASSERT_NE(shown, nullptr);
    EXPECT_TRUE(shown->isVisible());
    shown->close();
}

TEST_F(ShellContributions, PanelsOfOtherModulesStayAsTheyAre) {
    app_.add("test.first", R"(
[[contributes.bottomPanelTabs]]
id = "first.tab"
title = "First"
)");
    ads::CDockWidget* first = panel(QStringLiteral("tab:first.tab"));
    ASSERT_NE(first, nullptr);
    first->toggleView(false);  // Closed by the user.
    app_.add("test.second", R"(
[[contributes.bottomPanelTabs]]
id = "second.tab"
title = "Second"
)");
    EXPECT_EQ(panel(QStringLiteral("tab:first.tab")), first);
    EXPECT_TRUE(first->isClosed());
    EXPECT_FALSE(panel(QStringLiteral("tab:second.tab"))->isClosed());
}

TEST_F(ShellContributions, ARestoredLayoutKeepsTheTabsOfTheirConditions) {
    window_->save_workspace(QStringLiteral("Before the module"));
    app_.add("test.tab", R"(
[[contributes.bottomPanelTabs]]
id = "tab.log"
title = "Log"
)");
    ASSERT_TRUE(window_->open_workspace(QStringLiteral("Before the module")));
    // The layout did not know the tab; its condition (none) says it shows.
    EXPECT_FALSE(panel(QStringLiteral("tab:tab.log"))->isClosed());
}

TEST_F(ShellContributions, TitlesFollowTheLanguage) {
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    ASSERT_TRUE(QDir(folder.path()).mkpath(QStringLiteral("l10n")));
    const auto write = [&](const char* language, const std::string& text) {
        QFile file(QDir(folder.path()).filePath(QStringLiteral("l10n/%1.json").arg(language)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromStdString(
            R"({"format": "stresslab.l10n", "formatVersion": 1, "strings": {"run": ")" + text +
            "\"}}"));
    };
    write("en", "Run");
    write("ru", utf8(u8"Запустить"));
    app_.add("test.words", R"(
[[contributes.commands]]
id = "words.run"
title = "%run%"
[[contributes.menus]]
location = "solve"
command = "words.run"
[[contributes.localization]]
language = "en"
path = "l10n/en.json"
[[contributes.localization]]
language = "ru"
path = "l10n/ru.json"
)",
             std::filesystem::path(folder.path().toStdU16String()));
    EXPECT_EQ(shown_items(menu(QStringLiteral("menu.solve"))), QStringList({"Run"}));
    app_.localization.set_language("ru");
    EXPECT_EQ(shown_items(menu(QStringLiteral("menu.solve"))),
              QStringList({q(utf8(u8"Запустить"))}));
    EXPECT_EQ(menu(QStringLiteral("menu.solve"))->title(), q(utf8(u8"Расчёт")));
    EXPECT_EQ(find_action(bar(zone::stage_bar), QStringLiteral("stage.steps"))->text(),
              q(utf8(u8"Шаги и нагрузки")));
}

}  // namespace
