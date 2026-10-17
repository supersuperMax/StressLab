#include "fem/shell/main_window.h"

#include "fem/core/l10n/localization.h"
#include "fem/core/product.h"
#include "fem/shell/about_dialog.h"
#include "fem/shell/brand.h"
#include "fem/shell/command_palette.h"
#include "fem/shell/keys.h"
#include "fem/shell/problems.h"
#include "fem/shell/strings.h"
#include "fem/shell/theme.h"
#include "fem/shell/workspaces.h"

#include "shell_test_support.h"
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockSplitter.h>
#include <DockWidget.h>
#include <DockWidgetTab.h>
#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSplitterHandle>
#include <QToolBar>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace zone = fem::shell::zone;
using fem::shell::MainWindow;
using fem::shell::testing::find_action;

// The zones shown when the window opens; the settings are closed until opened.
const std::vector<std::string_view> all_zones{
    zone::stage_bar,   zone::activity_bar, zone::viewport,       zone::model_tree, zone::properties,
    zone::prompt_line, zone::journal,      zone::python_console, zone::jobs,       zone::problems};
const std::vector<std::string_view> bottom_panel{zone::journal, zone::python_console, zone::jobs,
                                                 zone::problems};

QString q(std::string_view text) {
    return fem::shell::to_qstring(text);
}

QMenu* menu(const MainWindow& window, const char* name) {
    return window.menuBar()->findChild<QMenu*>(QString::fromLatin1(name));
}

class ShellWindow : public ::testing::Test {
protected:
    void SetUp() override {
        fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
        window_ = std::make_unique<MainWindow>(app_.workbench());
        window_->show();
        QApplication::processEvents();
    }
    void TearDown() override { window_.reset(); }

    ads::CDockWidget* dock(std::string_view id) const {
        return qobject_cast<ads::CDockWidget*>(window_->zone_widget(id));
    }

    fem::shell::testing::Application app_;
    fem::l10n::Localization& localization_ = app_.localization;
    fem::shell::WorkspaceStore& workspaces_ = app_.workspaces;
    std::unique_ptr<MainWindow> window_;
};

TEST_F(ShellWindow, HasEveryZone) {
    for (const std::string_view id : all_zones) {
        QWidget* widget = window_->zone_widget(id);
        ASSERT_NE(widget, nullptr) << id;
        EXPECT_EQ(widget->objectName(), q(id));
        EXPECT_TRUE(window_->is_zone_shown(id)) << id;
    }
    EXPECT_EQ(window_->zone_widget("nothing"), nullptr);
    EXPECT_NE(qobject_cast<QToolBar*>(window_->zone_widget(zone::stage_bar)), nullptr);
    EXPECT_NE(qobject_cast<QToolBar*>(window_->zone_widget(zone::activity_bar)), nullptr);
    EXPECT_EQ(window_->toolBarArea(qobject_cast<QToolBar*>(window_->zone_widget(zone::stage_bar))),
              Qt::TopToolBarArea);
    EXPECT_EQ(
        window_->toolBarArea(qobject_cast<QToolBar*>(window_->zone_widget(zone::activity_bar))),
        Qt::LeftToolBarArea);
}

TEST_F(ShellWindow, IsNamedAfterTheProductAndHasItsIcon) {
    EXPECT_EQ(window_->windowTitle(), q(fem::kProductName));
    EXPECT_FALSE(window_->windowIcon().isNull());
    EXPECT_EQ(window_->windowIcon().pixmap(QSize(16, 16)).toImage(),
              fem::shell::brand::render_svg(fem::shell::brand::symbol_small_file, QSize(16, 16))
                  .toImage());
}

TEST_F(ShellWindow, TheBottomPanelHasFourTabsWithTheJournalInFront) {
    ads::CDockAreaWidget* area = dock(zone::journal)->dockAreaWidget();
    for (const std::string_view id : bottom_panel) {
        EXPECT_EQ(dock(id)->dockAreaWidget(), area) << id;
    }
    EXPECT_EQ(area->dockWidgetsCount(), 4);
    EXPECT_EQ(area->currentDockWidget(), dock(zone::journal));
    // The prompt line is a strip of its own above it, the tree and the properties are not there.
    EXPECT_NE(dock(zone::prompt_line)->dockAreaWidget(), area);
    EXPECT_NE(dock(zone::model_tree)->dockAreaWidget(), dock(zone::properties)->dockAreaWidget());
    EXPECT_TRUE(dock(zone::viewport)->isCentralWidget());
}

TEST_F(ShellWindow, EveryZoneButTheViewHidesFromTheViewMenu) {
    const QList<QAction*> view_actions = menu(*window_, "menu.view")->actions();
    for (const std::string_view id : all_zones) {
        QAction* action = window_->zone_action(id);
        if (id == zone::viewport) {
            EXPECT_EQ(action, nullptr);
            continue;
        }
        ASSERT_NE(action, nullptr) << id;
        EXPECT_TRUE(view_actions.contains(action)) << id;
        EXPECT_TRUE(action->isChecked()) << id;
        action->trigger();
        EXPECT_FALSE(window_->is_zone_shown(id)) << id;
        EXPECT_FALSE(action->isChecked()) << id;
        action->trigger();
        EXPECT_TRUE(window_->is_zone_shown(id)) << id;
    }
}

TEST_F(ShellWindow, PanelsUseTheColoursOfTheTheme) {
    // The style sheet of ADS would draw these in palette(dark): heavy splitters on the light
    // theme, unreadable inactive tabs on the dark one.
    for (const Qt::ColorScheme scheme : {Qt::ColorScheme::Light, Qt::ColorScheme::Dark}) {
        fem::shell::apply_theme(*qApp, scheme);
        MainWindow window(app_.workbench());
        window.show();
        QApplication::processEvents();
        const QPalette palette = fem::shell::theme_palette(scheme);
        const auto splitters = window.dock_manager()->findChildren<ads::CDockSplitter*>();
        ASSERT_FALSE(splitters.isEmpty());
        for (ads::CDockSplitter* splitter : splitters) {
            for (int i = 1; i < splitter->count(); ++i) {
                const QImage handle = splitter->handle(i)->grab().toImage();
                EXPECT_EQ(QColor(handle.pixel(handle.width() / 2, handle.height() / 2)),
                          palette.color(QPalette::Mid));
            }
        }
        auto* console = qobject_cast<ads::CDockWidget*>(window.zone_widget(zone::python_console));
        auto* journal = qobject_cast<ads::CDockWidget*>(window.zone_widget(zone::journal));
        const auto* inactive = console->tabWidget()->findChild<QLabel*>();
        const auto* active = journal->tabWidget()->findChild<QLabel*>();
        ASSERT_NE(inactive, nullptr);
        ASSERT_NE(active, nullptr);
        EXPECT_EQ(inactive->palette().color(QPalette::WindowText),
                  palette.color(QPalette::PlaceholderText));
        EXPECT_EQ(active->palette().color(QPalette::WindowText),
                  palette.color(QPalette::WindowText));
    }
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
}

TEST_F(ShellWindow, PanelsAndBarsCanBeMoved) {
    for (const std::string_view id : all_zones) {
        if (auto* bar = qobject_cast<QToolBar*>(window_->zone_widget(id))) {
            EXPECT_TRUE(bar->isMovable()) << id;
        } else if (id != zone::viewport) {
            EXPECT_TRUE(dock(id)->features().testFlag(ads::CDockWidget::DockWidgetMovable)) << id;
            EXPECT_TRUE(dock(id)->features().testFlag(ads::CDockWidget::DockWidgetClosable)) << id;
        }
    }
}

TEST_F(ShellWindow, WithoutModulesHasOnlyTheShellMenusAndTheStandardStages) {
    // Commands come from modules: the shell adds none (CLAUDE.md, rule 4). The activity bar has
    // only the activities of the shell: Model, History, Modules, Settings (docs/interface-design.md
    // §4, §12).
    QStringList activities;
    for (QAction* action :
         qobject_cast<QToolBar*>(window_->zone_widget(zone::activity_bar))->actions()) {
        activities.push_back(action->objectName());
        EXPECT_FALSE(action->icon().isNull()) << action->objectName().toStdString();
    }
    EXPECT_EQ(activities,
              QStringList({"view.modelTree", "view.history", "view.extensions", "view.settings"}));
    EXPECT_FALSE(window_->context_toolbar()->isVisibleTo(window_.get()));
    // The stage bar has the standard stages, in their order, then the variant (task 2.6), which
    // shows with a project folder.
    QStringList stages;
    for (QAction* action :
         qobject_cast<QToolBar*>(window_->zone_widget(zone::stage_bar))->actions()) {
        stages.push_back(action->objectName());
    }
    EXPECT_EQ(stages,
              QStringList({"stage.geometry", "stage.mesh", "stage.materials", "stage.steps",
                           "stage.solve", "stage.results", "stageBar.spacer", "stageBar.variant"}));
    EXPECT_FALSE(
        find_action(window_->zone_widget(zone::stage_bar), QStringLiteral("stageBar.variant"))
            ->isVisible());
    std::set<QString> named;
    for (QAction* top : window_->menuBar()->actions()) {
        ASSERT_NE(top->menu(), nullptr);
        named.insert(top->menu()->objectName());
        for (QAction* action : top->menu()->actions()) {
            if (!action->isSeparator() && action->menu() == nullptr) {
                named.insert(action->objectName());
            }
        }
    }
    std::set<QString> expected{"menu.file",
                               "app.settings",
                               "app.quit",
                               "menu.edit",
                               "app.undo",
                               "app.redo",
                               "menu.view",
                               "view.fullScreen",
                               "view.commandPalette",
                               "view.settings",
                               "menu.help",
                               "app.about",
                               "view.extensions",
                               "view.history",
                               "journal.saveAsScript",
                               "project.clean",
                               "project.open",
                               "project.save",
                               "project.saveAs",
                               "history.commitVersion"};
    for (const std::string_view id : all_zones) {
        if (id != zone::viewport) {
            expected.insert("view." + q(id));
        }
    }
    EXPECT_EQ(named, expected);
}

TEST_F(ShellWindow, SettingsOpenFromTheMenuAsATabNextToTheProperties) {
    ads::CDockWidget* settings = dock(zone::settings);
    ASSERT_NE(settings, nullptr);
    EXPECT_FALSE(window_->is_zone_shown(zone::settings));
    EXPECT_EQ(settings->dockAreaWidget(), dock(zone::properties)->dockAreaWidget());
    window_->findChild<QAction*>(QStringLiteral("app.settings"))->trigger();
    EXPECT_TRUE(window_->is_zone_shown(zone::settings));
    EXPECT_EQ(settings->dockAreaWidget()->currentDockWidget(), settings);
    EXPECT_TRUE(window_->zone_action(zone::settings)->isChecked());
    EXPECT_EQ(settings->windowTitle(), QStringLiteral("Settings"));
}

TEST_F(ShellWindow, ApplicationItemsUseTheRolesAndKeysOfTheOs) {
    auto* quit = window_->findChild<QAction*>(QStringLiteral("app.quit"));
    auto* about = window_->findChild<QAction*>(QStringLiteral("app.about"));
    ASSERT_NE(quit, nullptr);
    ASSERT_NE(about, nullptr);
    EXPECT_EQ(quit->menuRole(), QAction::QuitRole);
    EXPECT_EQ(about->menuRole(), QAction::AboutRole);
    auto* settings = window_->findChild<QAction*>(QStringLiteral("app.settings"));
    ASSERT_NE(settings, nullptr);
    EXPECT_EQ(settings->menuRole(), QAction::PreferencesRole);
    EXPECT_EQ(settings->shortcut(), fem::shell::key_sequence("Mod+,"));
    EXPECT_EQ(quit->shortcut(), fem::shell::key_sequence("Mod+Q"));
    EXPECT_EQ(window_->findChild<QAction*>(QStringLiteral("view.fullScreen"))->shortcut(),
              QKeySequence(QKeySequence::FullScreen));
    EXPECT_FALSE(QApplication::testAttribute(Qt::AA_DontUseNativeMenuBar));
}

TEST_F(ShellWindow, WorkspacesKeepLayouts) {
    const QString name = QStringLiteral("Постобработка");
    window_->save_workspace(name);
    EXPECT_EQ(window_->current_workspace(), name);
    EXPECT_TRUE(workspaces_.find(name));

    window_->zone_action(zone::properties)->trigger();
    window_->zone_action(zone::activity_bar)->trigger();
    ASSERT_FALSE(window_->is_zone_shown(zone::properties));
    window_->save_workspace(QStringLiteral("Без свойств"));

    ASSERT_TRUE(window_->open_workspace(name));
    EXPECT_TRUE(window_->is_zone_shown(zone::properties));
    EXPECT_TRUE(window_->is_zone_shown(zone::activity_bar));
    EXPECT_EQ(window_->current_workspace(), name);

    ASSERT_TRUE(window_->open_workspace(QStringLiteral("Без свойств")));
    EXPECT_FALSE(window_->is_zone_shown(zone::properties));
    EXPECT_FALSE(window_->is_zone_shown(zone::activity_bar));
    EXPECT_FALSE(window_->open_workspace(QStringLiteral("Нет такого")));
}

TEST_F(ShellWindow, ResetPutsEverythingBack) {
    window_->save_workspace(QStringLiteral("Свой"));
    window_->zone_action(zone::model_tree)->trigger();
    window_->zone_action(zone::stage_bar)->trigger();
    window_->reset_layout();
    EXPECT_TRUE(window_->is_zone_shown(zone::model_tree));
    EXPECT_TRUE(window_->is_zone_shown(zone::stage_bar));
    EXPECT_TRUE(window_->current_workspace().isEmpty());
}

TEST_F(ShellWindow, TheWorkspaceMenuListsWorkspaces) {
    window_->save_workspace(QStringLiteral("Сетка"));
    window_->save_workspace(QStringLiteral("Результаты"));
    QMenu* workspaces = menu(*window_, "menu.workspaces");
    ASSERT_NE(workspaces, nullptr);
    Q_EMIT workspaces->aboutToShow();
    QStringList titles;
    QString checked;
    for (QAction* action : workspaces->actions()) {
        if (action->isCheckable()) {
            titles << action->text();
            if (action->isChecked()) {
                checked = action->text();
            }
        }
    }
    EXPECT_EQ(titles, QStringList({"Default", "Результаты", "Сетка"}));
    EXPECT_EQ(checked, QStringLiteral("Результаты"));

    auto* remove = workspaces->findChild<QAction*>(QStringLiteral("workspace.delete"));
    if (remove == nullptr) {
        for (QAction* action : workspaces->actions()) {
            if (action->objectName() == QStringLiteral("workspace.delete")) {
                remove = action;
            }
        }
    }
    ASSERT_NE(remove, nullptr);
    EXPECT_TRUE(remove->isEnabled());
    EXPECT_EQ(remove->text(), QStringLiteral("Delete Workspace “Результаты”"));
    remove->trigger();
    EXPECT_FALSE(workspaces_.find(QStringLiteral("Результаты")));
    EXPECT_TRUE(window_->current_workspace().isEmpty());
}

TEST_F(ShellWindow, ReopensWithTheLayoutItWasClosedWith) {
    window_->zone_action(zone::properties)->trigger();
    window_->zone_action(zone::jobs)->trigger();
    window_->close();
    ASSERT_TRUE(workspaces_.session());

    MainWindow reopened(app_.workbench());
    reopened.show();
    QApplication::processEvents();
    EXPECT_FALSE(reopened.is_zone_shown(zone::properties));
    EXPECT_FALSE(reopened.is_zone_shown(zone::jobs));
    EXPECT_TRUE(reopened.is_zone_shown(zone::model_tree));
    // The layout it was built with stays the default.
    reopened.reset_layout();
    EXPECT_TRUE(reopened.is_zone_shown(zone::properties));
}

TEST_F(ShellWindow, TextsFollowTheLanguage) {
    EXPECT_EQ(menu(*window_, "menu.view")->title(), QStringLiteral("&View"));
    EXPECT_EQ(dock(zone::model_tree)->windowTitle(), QStringLiteral("Model Tree"));
    EXPECT_EQ(window_->prompt(), QStringLiteral("Ready"));

    localization_.set_language("ru");
    EXPECT_EQ(menu(*window_, "menu.view")->title(), QStringLiteral("&Вид"));
    EXPECT_EQ(menu(*window_, "menu.file")->title(), QStringLiteral("&Файл"));
    EXPECT_EQ(dock(zone::model_tree)->windowTitle(), QStringLiteral("Дерево модели"));
    EXPECT_EQ(dock(zone::python_console)->windowTitle(), QStringLiteral("Консоль Python"));
    EXPECT_EQ(window_->zone_action(zone::journal)->text(), QStringLiteral("Журнал"));
    EXPECT_EQ(window_->zone_widget(zone::stage_bar)->windowTitle(),
              QStringLiteral("Полоса этапов"));
    EXPECT_EQ(window_->prompt(), QStringLiteral("Готово"));
    EXPECT_EQ(window_->findChild<QAction*>(QStringLiteral("app.about"))->text(),
              "О программе " + q(fem::kProductName));
}

TEST_F(ShellWindow, ThePromptLineShowsHints) {
    window_->set_prompt(QStringLiteral("Выберите грани для закрепления."));
    EXPECT_EQ(window_->prompt(), QStringLiteral("Выберите грани для закрепления."));
    window_->set_prompt(QString());
    EXPECT_EQ(window_->prompt(), QStringLiteral("Ready"));
}

TEST_F(ShellWindow, AboutOpensOverTheWindow) {
    fem::shell::AboutDialog* about = window_->show_about();
    ASSERT_NE(about, nullptr);
    EXPECT_TRUE(about->isVisible());
    EXPECT_EQ(about->parentWidget(), window_.get());
    EXPECT_EQ(about->logo_file(), fem::shell::brand::logo_color_file);
    // The theme of the system changes while the dialog is open.
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Dark);
    QApplication::processEvents();
    EXPECT_EQ(about->logo_file(), fem::shell::brand::logo_reversed_file);
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
    about->close();
}

TEST_F(ShellWindow, EditUndoesAndRedoesStepsOfTheCommandBus) {
    auto* undo = window_->findChild<QAction*>(QStringLiteral("app.undo"));
    auto* redo = window_->findChild<QAction*>(QStringLiteral("app.redo"));
    ASSERT_NE(undo, nullptr);
    ASSERT_NE(redo, nullptr);
    EXPECT_FALSE(undo->isEnabled());
    EXPECT_FALSE(redo->isEnabled());
    EXPECT_EQ(undo->text(), QStringLiteral("Undo"));
    EXPECT_EQ(undo->shortcuts(), QKeySequence::keyBindings(QKeySequence::Undo));

    // Objects of a disabled module cannot be undone: the module is there.
    app_.add("test.model", "");
    (void)app_.bus.execute("model.create", {{"type", "item"}, {"owner", "test.model"}});
    EXPECT_TRUE(undo->isEnabled());
    EXPECT_EQ(undo->text(), QStringLiteral("Undo model.create"));

    undo->trigger();
    EXPECT_FALSE(app_.bus.can_undo());
    EXPECT_FALSE(undo->isEnabled());
    EXPECT_TRUE(redo->isEnabled());
    EXPECT_EQ(redo->text(), QStringLiteral("Redo model.create"));
    redo->trigger();
    EXPECT_TRUE(app_.bus.can_undo());
    EXPECT_FALSE(redo->isEnabled());
}

TEST_F(ShellWindow, ACrashedModuleShowsInProblems) {
    app_.add("test.crashy", R"(
[[contributes.commands]]
id = "crashy.go"
title = "Go"
)");
    EXPECT_EQ(dock(zone::problems)->windowTitle(), QStringLiteral("Problems"));
    app_.manager.activate("test.crashy");
    ASSERT_TRUE(app_.runtime->hosts.contains("test.crashy"));
    app_.runtime->hosts["test.crashy"]->report_crash(
        {.reason = fem::extension::CrashReason::crashed,
         .restarted = true,
         .count = 1,
         .details = "exit code 3"});
    ASSERT_EQ(window_->problems().count(), 1);
    const QString message = window_->problems().messages().front();
    EXPECT_TRUE(message.contains(QStringLiteral("Module test.crashy"))) << message.toStdString();
    EXPECT_EQ(window_->prompt(), message);
    EXPECT_EQ(dock(zone::problems)->windowTitle(), QStringLiteral("Problems (1)"));
}

TEST_F(ShellWindow, AModuleThatCannotStartShowsInProblems) {
    app_.runtime->broken.insert("test.broken");
    app_.add("test.broken", R"(
[[contributes.commands]]
id = "broken.go"
title = "Go"
)");
    window_->palette().reload();
    window_->palette().activate(window_->palette().row(QStringLiteral("broken.go")));
    ASSERT_EQ(window_->problems().count(), 1);
    EXPECT_EQ(window_->problems().messages().front(),
              QStringLiteral("The module “Module test.broken” could not start: broken on purpose"));
    // The prompt line says that the command failed.
    EXPECT_TRUE(window_->prompt().startsWith(QStringLiteral("“Go” failed: ")))
        << window_->prompt().toStdString();
}

}  // namespace
