// Acceptance of stage 1 in the window (task 1.15): the module fem.demo the application ships adds
// its stage, its button on the context toolbar (on its stage only), its tab, its node in the tree
// with a form, and its section of settings; disabled in the module manager, all of it goes away
// while its node stays in the model, opaque; enabled again, all of it comes back. The switch is
// written to the journal. Its code runs in the fake runtime of the tests.

#include "fem/core/command/model_commands.h"
#include "fem/core/command/value.h"
#include "fem/core/model/object_id.h"
#include "fem/shell/contribution_ui.h"
#include "fem/shell/extensions_panel.h"
#include "fem/shell/form_widget.h"
#include "fem/shell/main_window.h"
#include "fem/shell/model_tree.h"
#include "fem/shell/property_panel.h"
#include "fem/shell/settings_panel.h"
#include "fem/shell/theme.h"

#include "shell_test_support.h"
#include <DockManager.h>
#include <DockWidget.h>
#include <FloatingDockContainer.h>
#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QMessageBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QToolBar>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace zone = fem::shell::zone;
using fem::extension::ExtensionState;
using fem::model::ObjectId;
using fem::shell::MainWindow;
using fem::shell::testing::find_action;
using fem::shell::testing::q;

void settle() {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

class DemoAcceptance : public ::testing::Test {
protected:
    void SetUp() override {
        fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
        app_.add_demo();

        journaling_ =
            app_.bus.on_journal([this](std::string_view entry) { journal_.emplace_back(entry); });
        window_ = std::make_unique<MainWindow>(app_.workbench());
        window_->show();
        settle();
    }
    void TearDown() override { window_.reset(); }

    [[nodiscard]] QToolBar* bar(std::string_view id) const {
        return qobject_cast<QToolBar*>(window_->zone_widget(id));
    }
    [[nodiscard]] QStringList stages() const {
        QStringList ids;
        for (const QAction* action : bar(zone::stage_bar)->actions()) {
            // Not the switcher of variants at the end of the bar (task 2.6).
            if (action->isVisible() && action->objectName().startsWith(QStringLiteral("stage."))) {
                ids.push_back(action->objectName().mid(6));  // Without "stage.".
            }
        }
        return ids;
    }
    void open_stage(const QString& id) const {
        QAction* stage = find_action(bar(zone::stage_bar), QStringLiteral("stage.") + id);
        ASSERT_NE(stage, nullptr);
        stage->trigger();
        settle();
    }
    /// The button of the command on the context toolbar, if it shows.
    [[nodiscard]] QAction* button() const {
        for (QAction* action : window_->context_toolbar()->actions()) {
            if (action->objectName() == QStringLiteral("demo.add_note") && action->isVisible()) {
                return action;
            }
        }
        return nullptr;
    }
    [[nodiscard]] ads::CDockWidget* tab() const {
        return window_->dock_manager()->findDockWidget(QStringLiteral("tab:demo.notes"));
    }
    /// The titles of the top rows of the model tree.
    [[nodiscard]] QStringList rows() const {
        QStringList out;
        const QAbstractItemModel& proxy = window_->model_tree().proxy();
        for (int row = 0; row < proxy.rowCount(); ++row) {
            out.push_back(proxy.index(row, 0).data().toString());
        }
        return out;
    }
    /// The switch of the module in the module manager is on.
    [[nodiscard]] bool manager_toggle_checked() const {
        const QCheckBox* toggle = window_->extensions().toggle("fem.demo");
        return toggle != nullptr && toggle->isChecked();
    }
    [[nodiscard]] bool has_setting() const {
        return window_->settings().form().editor("demo.defaultText") != nullptr;
    }

    /// Everything the module adds shows, its node with its form.
    void expect_present(ObjectId note) const {
        EXPECT_EQ(stages(), QStringList({"geometry", "mesh", "materials", "steps", "solve",
                                         "results", "demo.review"}));
        // The button shows on the stage of the module only.
        open_stage(QStringLiteral("results"));
        EXPECT_EQ(button(), nullptr);
        open_stage(QStringLiteral("demo.review"));
        ASSERT_NE(button(), nullptr);
        EXPECT_EQ(button()->text(), QStringLiteral("Add a Note"));
        ASSERT_NE(tab(), nullptr);
        EXPECT_EQ(tab()->windowTitle(), QStringLiteral("Notes"));
        EXPECT_EQ(rows(), QStringList({"#1"}));
        EXPECT_TRUE(window_->model_tree()
                        .model()
                        .index_of(note)
                        .data(Qt::ToolTipRole)
                        .toString()
                        .startsWith(QStringLiteral("Note — module “Demo Module”")));
        window_->model_tree().select(note);
        EXPECT_EQ(window_->properties().object(), note);
        EXPECT_NE(window_->properties().form().editor("text"), nullptr);
        EXPECT_NE(window_->properties().form().editor("priority"), nullptr);
        EXPECT_TRUE(has_setting());
    }

    fem::shell::testing::Application app_;
    std::vector<std::string> journal_;
    std::unique_ptr<MainWindow> window_;

private:
    fem::Subscription journaling_;
};

TEST_F(DemoAcceptance, TheModuleGoesAwayAndComesBackWithItsNodeKeptOpaque) {
    // The button runs the command of the module.
    open_stage(QStringLiteral("demo.review"));
    ASSERT_NE(button(), nullptr);
    button()->trigger();
    EXPECT_EQ(app_.runtime->log, (std::vector<std::string>{"execute demo.add_note"}));
    // The note the code of the module would create.
    const ObjectId note(static_cast<std::uint64_t>(
        app_.bus
            .execute(fem::command::model_commands::create,
                     {{"type", "demo_note"},
                      {"owner", "fem.demo"},
                      {"name", "#1"},
                      {"properties", fem::command::Value::Dict{{"text", "TODO"}}}})
            .value.as_integer()));
    expect_present(note);
    journal_.clear();

    // Disabled in the module manager: it asks about the node it hides.
    fem::shell::ExtensionsPanel& manager = window_->extensions();
    manager.set_enabled("fem.demo", false);
    settle();
    QMessageBox* question = nullptr;
    for (QMessageBox* box :
         manager.findChildren<QMessageBox*>(QStringLiteral("disableModuleDialog"))) {
        if (box->isVisible()) {
            question = box;
        }
    }
    ASSERT_NE(question, nullptr);
    EXPECT_TRUE(question->text().contains(QStringLiteral("1 node")));
    question->findChild<QPushButton*>(QStringLiteral("accept"))->click();
    settle();

    EXPECT_EQ(app_.manager.get("fem.demo").state, ExtensionState::disabled);
    EXPECT_EQ(stages(),
              QStringList({"geometry", "mesh", "materials", "steps", "solve", "results"}));
    EXPECT_EQ(button(), nullptr);
    EXPECT_EQ(tab(), nullptr);
    EXPECT_TRUE(rows().isEmpty());
    EXPECT_FALSE(window_->properties().object().is_valid());
    EXPECT_FALSE(has_setting());
    EXPECT_EQ(app_.bus.find_command("demo.add_note"), nullptr);
    ASSERT_EQ(app_.document.size(), 1U) << "the node stays in the model";
    EXPECT_TRUE(app_.document.is_opaque(app_.document.get(note)));
    EXPECT_EQ(journal_, (std::vector<std::string>{"stresslab.extensions.disable(\"fem.demo\")"}));

    // Enabled again: everything is back, the node too.
    manager.set_enabled("fem.demo", true);
    settle();
    EXPECT_EQ(app_.manager.get("fem.demo").state, ExtensionState::enabled);
    EXPECT_FALSE(app_.document.is_opaque(app_.document.get(note)));
    expect_present(note);
    EXPECT_EQ(journal_, (std::vector<std::string>{"stresslab.extensions.disable(\"fem.demo\")",
                                                  "stresslab.extensions.enable(\"fem.demo\")"}));
}

TEST_F(DemoAcceptance, DisablingWhileItsNodeIsSelectedClearsTheSelection) {
    const ObjectId note(static_cast<std::uint64_t>(
        app_.bus
            .execute(fem::command::model_commands::create,
                     {{"type", "demo_note"}, {"owner", "fem.demo"}, {"name", "#1"}})
            .value.as_integer()));
    open_stage(QStringLiteral("demo.review"));
    window_->model_tree().select(note);
    ASSERT_EQ(window_->properties().object(), note);
    (void)app_.manager.disable("fem.demo", fem::extension::Journaling::on);
    settle();
    EXPECT_FALSE(window_->model_tree().selected().is_valid());
    EXPECT_FALSE(window_->properties().object().is_valid());
    (void)app_.manager.enable("fem.demo", fem::extension::Journaling::on);
    settle();
    EXPECT_EQ(rows(), QStringList({"#1"}));
}

TEST_F(DemoAcceptance, SwitchingSeveralTimesBeforeTheEventLoopRunsIsSafe) {
    // One line of the console: `enable(...); disable(...)`. The dock system queues work for the
    // tab it has just added; the tab removed meanwhile must still be there when it runs.
    for (int round = 0; round < 3; ++round) {
        (void)app_.manager.disable("fem.demo", fem::extension::Journaling::on);
        (void)app_.manager.enable("fem.demo", fem::extension::Journaling::on);
    }
    (void)app_.manager.disable("fem.demo", fem::extension::Journaling::on);
    settle();
    EXPECT_EQ(tab(), nullptr);
    (void)app_.manager.enable("fem.demo", fem::extension::Journaling::on);
    settle();
    EXPECT_NE(tab(), nullptr);
}

TEST_F(DemoAcceptance, ALayoutSavedWithoutTheModuleKeepsItsTabInTheBottomPanel) {
    // The layout of the last session was saved before the module came (an update of the
    // application, a module installed since): its tab is not in it.
    (void)app_.manager.disable("fem.demo");
    settle();
    window_->save_workspace(QStringLiteral("Без модуля"));
    window_.reset();
    (void)app_.manager.enable("fem.demo");
    window_ = std::make_unique<MainWindow>(app_.workbench());
    window_->show();
    settle();
    ASSERT_TRUE(window_->open_workspace(QStringLiteral("Без модуля")));
    settle();

    ASSERT_NE(tab(), nullptr);
    EXPECT_FALSE(tab()->isFloating());
    EXPECT_FALSE(tab()->isClosed());
    ads::CDockWidget* journal = window_->dock_manager()->findDockWidget(q(zone::journal));
    EXPECT_EQ(tab()->dockAreaWidget(), journal->dockAreaWidget()) << "next to the journal";
    for (const ads::CFloatingDockContainer* floating : window_->dock_manager()->floatingWidgets()) {
        EXPECT_FALSE(floating->isVisible()) << "no window of its own";
    }
}

TEST_F(DemoAcceptance, TheSameHappensWhenTheModuleIsSwitchedFromOutsideTheWindow) {
    // As `stresslab.extensions.disable` in the console does it.
    (void)app_.manager.disable("fem.demo", fem::extension::Journaling::on);
    settle();
    EXPECT_EQ(tab(), nullptr);
    EXPECT_FALSE(stages().contains(QStringLiteral("demo.review")));
    EXPECT_FALSE(has_setting());
    EXPECT_FALSE(manager_toggle_checked());

    (void)app_.manager.enable("fem.demo", fem::extension::Journaling::on);
    settle();
    EXPECT_NE(tab(), nullptr);
    EXPECT_TRUE(stages().contains(QStringLiteral("demo.review")));
    EXPECT_TRUE(has_setting());
    EXPECT_TRUE(manager_toggle_checked());
}

}  // namespace
