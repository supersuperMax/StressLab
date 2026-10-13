// The command palette (task 1.12): every command, the unavailable ones grey with the reason,
// commands of disabled modules with "Enable".

#include "fem/shell/command_palette.h"

#include "fem/shell/keys.h"
#include "fem/shell/main_window.h"
#include "fem/shell/theme.h"

#include "shell_test_support.h"
#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTest>
#include <memory>
#include <string>
#include <vector>

namespace {

using fem::shell::CommandPalette;
using fem::shell::MainWindow;
using Kind = CommandPalette::RowKind;

class ShellPalette : public ::testing::Test {
protected:
    void SetUp() override {
        fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
        app_.add("test.alpha", R"(
[[contributes.commands]]
id = "alpha.run"
title = "Run Alpha"
category = "Alpha"
[[contributes.commands]]
id = "alpha.report"
title = "Report Alpha"
enablement = "model.hasMesh"
[[contributes.keybindings]]
command = "alpha.run"
key = "Mod+Shift+A"
)");
        (void)app_.install("test.beta", R"(
[[contributes.commands]]
id = "beta.export"
title = "Export to Beta"
)");
        window_ = std::make_unique<MainWindow>(app_.workbench());
        window_->show();
        QApplication::processEvents();
    }
    void TearDown() override { window_.reset(); }

    [[nodiscard]] CommandPalette& palette() const { return window_->palette(); }
    [[nodiscard]] QListWidgetItem* row(const char* id) const {
        return palette().row(QString::fromLatin1(id));
    }
    /// Ids of the rows shown.
    [[nodiscard]] QStringList rows() const {
        QStringList ids;
        for (int i = 0; i < palette().list()->count(); ++i) {
            ids.push_back(palette().list()->item(i)->data(Qt::UserRole).toString());
        }
        return ids;
    }
    [[nodiscard]] QWidget* row_widget(const char* id) const {
        return palette().list()->itemWidget(row(id));
    }

    fem::shell::testing::Application app_;
    std::unique_ptr<MainWindow> window_;
};

TEST_F(ShellPalette, OpensWithItsShortcut) {
    EXPECT_FALSE(palette().isVisible());
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    QTest::keyClick(window_.get(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_TRUE(palette().isVisible());
    EXPECT_TRUE(palette().filter()->text().isEmpty());
    palette().close();
    auto* action = window_->findChild<QAction*>(QStringLiteral("view.commandPalette"));
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->shortcut(), fem::shell::key_sequence("Mod+Shift+P"));
}

TEST_F(ShellPalette, ListsEveryCommandWithWhereItComesFrom) {
    window_->show_palette();
    // Commands of the enabled and the disabled module, and actions of the shell.
    for (const char* id : {"alpha.run", "alpha.report", "beta.export", "app.quit", "app.undo"}) {
        EXPECT_NE(row(id), nullptr) << id;
    }
    EXPECT_EQ(palette().row_kind(row("alpha.run")), Kind::run);
    EXPECT_EQ(palette().row_detail(row("alpha.run")), QStringLiteral("Module test.alpha"));
    EXPECT_EQ(row_widget("alpha.run")->findChild<QLabel*>(QStringLiteral("palette.title"))->text(),
              QStringLiteral("Alpha: Run Alpha"));
    EXPECT_EQ(
        row_widget("alpha.run")->findChild<QLabel*>(QStringLiteral("palette.shortcut"))->text(),
        fem::shell::shortcut_text(fem::shell::key_sequence("Mod+Shift+A")));
    // Grey, with the reason.
    EXPECT_EQ(palette().row_kind(row("alpha.report")), Kind::unavailable);
    EXPECT_EQ(palette().row_detail(row("alpha.report")),
              QStringLiteral("Requires a mesh in the model"));
    EXPECT_EQ(row_widget("alpha.report")
                  ->findChild<QLabel*>(QStringLiteral("palette.title"))
                  ->foregroundRole(),
              QPalette::PlaceholderText);
    // In a disabled module, with "Enable".
    EXPECT_EQ(palette().row_kind(row("beta.export")), Kind::enable);
    EXPECT_EQ(palette().row_detail(row("beta.export")),
              QStringLiteral("In the module “Module test.beta”, which is disabled"));
    EXPECT_NE(row_widget("beta.export")->findChild<QPushButton*>(QStringLiteral("palette.enable")),
              nullptr);
    EXPECT_EQ(row_widget("alpha.run")->findChild<QPushButton*>(QStringLiteral("palette.enable")),
              nullptr);
    // Nothing to undo yet.
    EXPECT_EQ(palette().row_kind(row("app.undo")), Kind::unavailable);
}

TEST_F(ShellPalette, TypingFiltersTheList) {
    window_->show_palette();
    QTest::keyClicks(palette().filter(), QStringLiteral("alpha"));
    EXPECT_EQ(rows(), QStringList({"alpha.run", "alpha.report"}));
    palette().filter()->clear();
    QTest::keyClicks(palette().filter(), QStringLiteral("BETA exp"));
    EXPECT_EQ(rows(), QStringList({"beta.export"}));
    palette().filter()->clear();
    QTest::keyClicks(palette().filter(), QStringLiteral("nothing like it"));
    EXPECT_TRUE(rows().isEmpty());
}

TEST_F(ShellPalette, EnterRunsTheSelectedCommand) {
    window_->show_palette();
    QTest::keyClicks(palette().filter(), QStringLiteral("run alpha"));
    ASSERT_EQ(rows(), QStringList({"alpha.run"}));
    QTest::keyClick(palette().filter(), Qt::Key_Return);
    EXPECT_FALSE(palette().isVisible());
    EXPECT_EQ(app_.runtime->log, std::vector<std::string>{"execute alpha.run"});
}

TEST_F(ShellPalette, AnUnavailableCommandDoesNotRun) {
    window_->show_palette();
    QTest::keyClicks(palette().filter(), QStringLiteral("report"));
    QTest::keyClick(palette().filter(), Qt::Key_Return);
    EXPECT_TRUE(palette().isVisible());
    EXPECT_TRUE(app_.runtime->log.empty());
}

TEST_F(ShellPalette, EnableEnablesTheModuleAndItsCommandBecomesAvailable) {
    window_->show_palette();
    auto* enable =
        row_widget("beta.export")->findChild<QPushButton*>(QStringLiteral("palette.enable"));
    ASSERT_NE(enable, nullptr);
    QTest::mouseClick(enable, Qt::LeftButton);
    QApplication::processEvents();
    EXPECT_TRUE(app_.manager.get("test.beta").enabled());
    EXPECT_TRUE(palette().isVisible());
    ASSERT_NE(row("beta.export"), nullptr);
    EXPECT_EQ(palette().row_kind(row("beta.export")), Kind::run);
    EXPECT_EQ(palette().list()->currentItem(), row("beta.export"));
    // Enter now runs it.
    QTest::keyClick(palette().filter(), Qt::Key_Return);
    EXPECT_EQ(app_.runtime->log, std::vector<std::string>{"execute beta.export"});
}

TEST_F(ShellPalette, AModuleThatCannotBeEnabledSaysWhy) {
    (void)app_.install("test.needy", R"(
[[contributes.commands]]
id = "needy.go"
title = "Go"
)",
                       {}, "requires = [\"test.missing>=1.0\"]\n");
    window_->show_palette();
    ASSERT_NE(row("needy.go"), nullptr);
    EXPECT_EQ(palette().row_kind(row("needy.go")), Kind::cannot_enable);
    EXPECT_TRUE(
        palette()
            .row_detail(row("needy.go"))
            .startsWith(QStringLiteral("The module “Module test.needy” cannot be enabled: ")))
        << palette().row_detail(row("needy.go")).toStdString();
    EXPECT_EQ(row_widget("needy.go")->findChild<QPushButton*>(QStringLiteral("palette.enable")),
              nullptr);
    palette().activate(row("needy.go"));
    EXPECT_FALSE(app_.manager.get("test.needy").enabled());
}

}  // namespace
