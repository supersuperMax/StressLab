// The "Python Console" and "Journal" tabs of the bottom panel (task 1.14). The console runs with a
// fake engine here; the embedded Python behind it is tested in tests/unit/python/console_test.cpp
// and python/tests/test_console.py.

#include "fem/core/command/journal.h"
#include "fem/shell/journal_panel.h"
#include "fem/shell/main_window.h"
#include "fem/shell/python_console.h"
#include "fem/shell/theme.h"

#include "shell_test_support.h"
#include <DockWidget.h>
#include <gtest/gtest.h>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCompleter>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QTest>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace zone = fem::shell::zone;
using fem::shell::ConsoleEngine;
using fem::shell::JournalPanel;
using fem::shell::MainWindow;
using fem::shell::PythonConsolePanel;
using fem::shell::testing::find_action;
using fem::shell::testing::utf8;

/// A console that knows a few lines: "x:" opens a block, "boom" fails, "print <text>" prints;
/// completion offers the names that start with the last word.
class FakeEngine final : public ConsoleEngine {
public:
    std::vector<QString> pushed;
    int resets = 0;
    QStringList names{"stresslab", "stresslab.cmd(", "stresslab.model", "stresslab.undo()"};

    [[nodiscard]] QString banner() const override { return QStringLiteral("Fake 1.0"); }

    Reply push(const QString& line) override {
        pushed.push_back(line);
        if (line.endsWith(QLatin1Char(':'))) {
            open_ = true;
            return {.more = true};
        }
        if (open_) {
            if (!line.isEmpty()) {
                return {.more = true};
            }
            open_ = false;
            return {.more = false, .output = {{false, QStringLiteral("block done\n")}}};
        }
        if (line == QLatin1String("boom")) {
            return {.output = {{true, QStringLiteral("Traceback\nRuntimeError: boom\n")}}};
        }
        if (line.startsWith(QLatin1String("print "))) {
            return {.output = {{false, line.mid(6) + QLatin1Char('\n')}}};
        }
        return {};
    }

    void reset() override {
        ++resets;
        open_ = false;
    }

    [[nodiscard]] QStringList complete(const QString& line) override {
        QStringList matches;
        for (const QString& name : names) {
            if (!line.isEmpty() && name.startsWith(line)) {
                matches.push_back(name);
            }
        }
        return matches;
    }

private:
    bool open_ = false;
};

std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class ShellConsole : public ::testing::Test {
protected:
    void SetUp() override {
        workbench_.console = &engine_;
        panel_ = std::make_unique<PythonConsolePanel>(workbench_);
        panel_->show();
        QApplication::processEvents();
    }

    fem::shell::testing::Application app_;
    FakeEngine engine_;
    fem::shell::Workbench workbench_ = app_.workbench();
    std::unique_ptr<PythonConsolePanel> panel_;
};

TEST_F(ShellConsole, RunsLinesAndShowsWhatTheyPrint) {
    EXPECT_TRUE(panel_->transcript().startsWith(QStringLiteral("Fake 1.0\n")));
    EXPECT_EQ(panel_->prompt(), QStringLiteral(">>>"));
    QLineEdit* input = panel_->input();
    // Qt Test types only Latin-1 keys: the Cyrillic text is put in, Enter is pressed.
    input->setText(QStringLiteral("print привет"));
    QTest::keyClick(input, Qt::Key_Return);
    EXPECT_EQ(engine_.pushed, std::vector<QString>{QStringLiteral("print привет")});
    EXPECT_TRUE(input->text().isEmpty());
    EXPECT_TRUE(panel_->transcript().endsWith(QStringLiteral(">>> print привет\nпривет")))
        << panel_->transcript().toStdString();

    panel_->run(QStringLiteral("boom"));
    EXPECT_TRUE(panel_->transcript().endsWith(QStringLiteral("RuntimeError: boom")));
}

TEST_F(ShellConsole, BlocksOfSeveralLinesChangeThePrompt) {
    panel_->run(QStringLiteral("for x in y:"));
    EXPECT_EQ(panel_->prompt(), QStringLiteral("..."));
    EXPECT_EQ(panel_->findChild<QLabel*>(QStringLiteral("consolePrompt"))->text(),
              QStringLiteral("..."));
    panel_->run(QStringLiteral("    pass"));
    EXPECT_EQ(panel_->prompt(), QStringLiteral("..."));
    panel_->run(QString());
    EXPECT_EQ(panel_->prompt(), QStringLiteral(">>>"));
    EXPECT_TRUE(panel_->transcript().endsWith(QStringLiteral("... \nblock done")));

    // Escape drops an unfinished block.
    panel_->run(QStringLiteral("if True:"));
    QTest::keyClicks(panel_->input(), QStringLiteral("half"));
    QTest::keyClick(panel_->input(), Qt::Key_Escape);
    EXPECT_EQ(engine_.resets, 1);
    EXPECT_EQ(panel_->prompt(), QStringLiteral(">>>"));
    EXPECT_TRUE(panel_->input()->text().isEmpty());
}

TEST_F(ShellConsole, UpAndDownGoThroughEarlierLines) {
    panel_->run(QStringLiteral("print a"));
    panel_->run(QStringLiteral("print b"));
    panel_->run(QStringLiteral("print b"));  // Repeated: kept once.
    panel_->run(QString());                  // Empty: not kept.
    EXPECT_EQ(panel_->history(), QStringList({"print a", "print b"}));
    QLineEdit* input = panel_->input();
    QTest::keyClicks(input, QStringLiteral("draft"));
    QTest::keyClick(input, Qt::Key_Up);
    EXPECT_EQ(input->text(), QStringLiteral("print b"));
    QTest::keyClick(input, Qt::Key_Up);
    EXPECT_EQ(input->text(), QStringLiteral("print a"));
    QTest::keyClick(input, Qt::Key_Up);
    EXPECT_EQ(input->text(), QStringLiteral("print a"));
    QTest::keyClick(input, Qt::Key_Down);
    EXPECT_EQ(input->text(), QStringLiteral("print b"));
    QTest::keyClick(input, Qt::Key_Down);
    EXPECT_EQ(input->text(), QStringLiteral("draft"));
}

TEST_F(ShellConsole, TabCompletesOneMatchAndOffersSeveral) {
    QLineEdit* input = panel_->input();
    input->setFocus();
    QTest::keyClicks(input, QStringLiteral("stresslab.u"));
    QTest::keyClick(input, Qt::Key_Tab);
    EXPECT_EQ(input->text(), QStringLiteral("stresslab.undo()"));
    EXPECT_FALSE(panel_->completer()->popup()->isVisible());
    // Tab did not move the focus.
    EXPECT_TRUE(input->hasFocus());

    input->setText(QStringLiteral("stres"));
    QTest::keyClick(input, Qt::Key_Tab);
    // What all matches share is put in, the rest is offered.
    EXPECT_EQ(input->text(), QStringLiteral("stresslab"));
    QAbstractItemView* popup = panel_->completer()->popup();
    ASSERT_TRUE(popup->isVisible());
    EXPECT_EQ(popup->model()->rowCount(), 4);
    popup->setCurrentIndex(popup->model()->index(2, 0));
    QTest::keyClick(popup, Qt::Key_Return);
    EXPECT_EQ(input->text(), QStringLiteral("stresslab.model"));

    input->setText(QStringLiteral("nothing"));
    QTest::keyClick(input, Qt::Key_Tab);
    EXPECT_EQ(input->text(), QStringLiteral("nothing"));
}

TEST(ShellConsoleWithoutPython, SaysWhyAndTakesNoInput) {
    fem::shell::testing::Application app;
    fem::shell::Workbench workbench = app.workbench();
    workbench.console_unavailable = "no Python here";
    const PythonConsolePanel panel(workbench);
    EXPECT_TRUE(panel.transcript().contains(QStringLiteral("no Python here")));
    EXPECT_FALSE(panel.input()->isEnabled());
}

// --- The journal ---------------------------------------------------------------------------------

class ShellJournal : public ::testing::Test {
protected:
    ShellJournal() : folder_(fs::temp_directory_path() / fs::path(u8"журнал оболочки StressLab")) {
        std::error_code ignored;
        fs::remove_all(folder_, ignored);
        fs::create_directories(folder_);
        app_.add("test.journal", R"([[contributes.treeNodeTypes]]
type = "item"
title = "Item"
)");
        workbench_.journal = &journal_;
        workbench_.dialogs.save_file = [this](QWidget*, const QString&, const QString& name,
                                              const QString& filter) {
            asked_name_ = name;
            asked_filter_ = filter;
            return answer_;
        };
    }
    ~ShellJournal() override {
        std::error_code ignored;
        fs::remove_all(folder_, ignored);
    }
    ShellJournal(const ShellJournal&) = delete;
    ShellJournal& operator=(const ShellJournal&) = delete;
    ShellJournal(ShellJournal&&) = delete;
    ShellJournal& operator=(ShellJournal&&) = delete;

    void create(const std::string& name) {
        (void)app_.bus.execute("model.create",
                               {{"type", "item"}, {"owner", "test.journal"}, {"name", name}});
    }

    fs::path folder_;
    fem::shell::testing::Application app_;
    fem::command::SessionJournal journal_{app_.bus};
    fem::shell::Workbench workbench_ = app_.workbench();
    fs::path answer_;
    QString asked_name_;
    QString asked_filter_;
};

TEST_F(ShellJournal, ShowsTheEntriesOfTheSession) {
    create("before");
    JournalPanel panel(workbench_);
    EXPECT_EQ(panel.text().toStdString(), journal_.entries().front());
    create(utf8(u8"после"));
    (void)app_.bus.undo();
    const QStringList lines = panel.text().split(QLatin1Char('\n'));
    ASSERT_EQ(lines.size(), 3);
    EXPECT_TRUE(lines[1].startsWith(QStringLiteral("stresslab.cmd(\"model.create\"")));
    EXPECT_TRUE(lines[1].contains(QStringLiteral("после")));
    EXPECT_EQ(lines[2], QStringLiteral("stresslab.undo()"));
}

TEST_F(ShellJournal, SavesTheJournalAsAScript) {
    QStringList reports;
    JournalPanel panel(workbench_);
    panel.set_report([&](const QString& message) { reports.push_back(message); });
    create("one");
    create("two");

    // Cancelled: nothing is written.
    EXPECT_FALSE(panel.save_as_script());
    EXPECT_EQ(asked_name_, QStringLiteral("session.slpy"));
    EXPECT_TRUE(asked_filter_.contains(QStringLiteral("*.slpy")));
    EXPECT_TRUE(reports.isEmpty());

    // Without an extension the file gets .slpy.
    answer_ = folder_ / fs::path(u8"мой сценарий");
    panel.save_action()->trigger();
    const fs::path saved = folder_ / fs::path(u8"мой сценарий.slpy");
    ASSERT_TRUE(fs::exists(saved));
    EXPECT_EQ(read(saved), journal_.text());
    EXPECT_EQ(read(saved).rfind(fem::command::journal_header(), 0), 0U);
    ASSERT_EQ(reports.size(), 1);
    EXPECT_TRUE(reports[0].contains(QStringLiteral("мой сценарий.slpy")));

    // A file that cannot be written is reported.
    answer_ = folder_ / "missing" / "x.slpy";
    EXPECT_FALSE(panel.save_as_script());
    ASSERT_EQ(reports.size(), 2);
    EXPECT_NE(reports[1], reports[0]);
}

TEST_F(ShellJournal, TheWindowHasTheTabsAndTheMenuItem) {
    FakeEngine engine;
    workbench_.console = &engine;
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
    MainWindow window(workbench_);
    window.show();
    QApplication::processEvents();
    auto* journal = qobject_cast<ads::CDockWidget*>(window.zone_widget(zone::journal));
    auto* console = qobject_cast<ads::CDockWidget*>(window.zone_widget(zone::python_console));
    ASSERT_NE(journal, nullptr);
    ASSERT_NE(console, nullptr);
    EXPECT_EQ(journal->widget(), &window.journal());
    EXPECT_EQ(console->widget(), &window.console());

    // "File" → "Save Journal as Script…" saves what the console did.
    QMenu* file = window.menuBar()->findChild<QMenu*>(QStringLiteral("menu.file"));
    QAction* save = find_action(file, QStringLiteral("journal.saveAsScript"));
    ASSERT_NE(save, nullptr);
    EXPECT_EQ(save, window.save_journal_action());
    create("from the window");
    answer_ = folder_ / "window.slpy";
    save->trigger();
    EXPECT_EQ(read(folder_ / "window.slpy"), journal_.text());
    EXPECT_TRUE(window.prompt().contains(QStringLiteral("window.slpy")));
}

}  // namespace
