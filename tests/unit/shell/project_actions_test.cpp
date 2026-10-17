// Opening and saving the project folder in the window (task 2.1): the "File" menu, the title,
// autosave and the question about unsaved changes; "Clean project" (task 2.2).

#include "fem/shell/project_actions.h"

#include "fem/core/io/files.h"
#include "fem/core/model/document_editor.h"
#include "fem/core/model/mesh.h"
#include "fem/core/model/object.h"
#include "fem/core/settings/settings.h"
#include "fem/shell/main_window.h"
#include "fem/shell/problems.h"
#include "fem/storage/project.h"
#include "fem/storage/session.h"

#include "shell_test_support.h"
#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QMenu>
#include <QMenuBar>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <filesystem>
#include <functional>
#include <memory>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::shell::MainWindow;
using fem::shell::ProjectActions;
using fem::shell::UnsavedChanges;
using fem::shell::testing::q;
using fem::shell::testing::utf8;

class ShellProject : public ::testing::Test {
protected:
    ShellProject()
        : folder_(fs::temp_directory_path() / fs::path(u8"stresslab окно проекта")),
          project_(app_.bus) {
        std::error_code ignored;
        fs::remove_all(folder_, ignored);
        fs::create_directories(folder_);
        app_.document.set_owner_enabled("test.module", true);

        fem::shell::Workbench workbench = app_.workbench();
        workbench.project = &project_;
        workbench.dialogs.open_folder = [this](QWidget*, const QString&) { return chosen_; };
        workbench.dialogs.save_file = [this](QWidget*, const QString&, const QString& name,
                                             const QString&) {
            suggested_ = name;
            return chosen_;
        };
        workbench.ask_unsaved = [this](QWidget*, const QString& name) {
            asked_.push_back(name);
            return answer_;
        };
        window_ = std::make_unique<MainWindow>(workbench);
    }

    ~ShellProject() override {
        window_.reset();
        std::error_code ignored;
        fs::remove_all(folder_, ignored);
    }

    void add_note(const char* name) {
        (void)app_.bus.execute("model.create",
                               {{"type", "note"}, {"owner", "test.module"}, {"name", name}});
    }

    ProjectActions& actions() { return window_->project(); }

    fem::shell::testing::Application app_;
    fs::path folder_;
    fem::storage::ProjectSession project_;
    std::unique_ptr<MainWindow> window_;
    fs::path chosen_;
    QString suggested_;
    UnsavedChanges answer_ = UnsavedChanges::cancel;
    std::vector<QString> asked_;
};

TEST_F(ShellProject, FileMenuHasTheProjectActions) {
    QMenu* file = window_->menuBar()->findChild<QMenu*>(QStringLiteral("menu.file"));
    ASSERT_NE(file, nullptr);
    const QList<QAction*> items = file->actions();
    ASSERT_GE(items.size(), 5);
    EXPECT_EQ(items[0]->objectName(), QStringLiteral("project.open"));
    EXPECT_EQ(items[2]->objectName(), QStringLiteral("project.save"));
    EXPECT_EQ(items[3]->objectName(), QStringLiteral("project.saveAs"));
    EXPECT_EQ(items[4]->objectName(), QStringLiteral("history.commitVersion"));
    EXPECT_EQ(items[5]->objectName(), QStringLiteral("project.clean"));
    EXPECT_EQ(actions().clean_action()->text(), QStringLiteral("Clean Project"));
    EXPECT_EQ(actions().save_action()->shortcut(), QKeySequence(QKeySequence::Save));
    EXPECT_EQ(actions().open_action()->shortcut(), QKeySequence(QKeySequence::Open));
    EXPECT_TRUE(actions().save_as_action()->shortcut().isEmpty());
    EXPECT_EQ(actions().save_action()->text(), QStringLiteral("Save"));
}

TEST_F(ShellProject, TitleShowsTheProjectAndUnsavedChanges) {
    EXPECT_EQ(window_->windowTitle(), QStringLiteral("Untitled[*] — StressLab"));
    EXPECT_FALSE(window_->isWindowModified());
    add_note("a");
    EXPECT_TRUE(window_->isWindowModified());

    chosen_ = folder_ / fs::path(u8"крыло");
    actions().save_action()->trigger();  // No folder yet: asks for one.
    EXPECT_EQ(suggested_, QStringLiteral("Untitled.slproj"));
    EXPECT_TRUE(fem::storage::is_project_folder(folder_ / fs::path(u8"крыло.slproj")));
    // With a folder the title names the variant too (task 2.6).
    EXPECT_EQ(window_->windowTitle(), q(utf8(u8"крыло — Main variant[*] — StressLab")));
    EXPECT_FALSE(window_->isWindowModified());
}

TEST_F(ShellProject, OpenAsksAboutUnsavedChanges) {
    add_note("saved");
    project_.save_as(folder_ / "p");
    add_note("unsaved");

    chosen_ = folder_ / "p.slproj";
    answer_ = UnsavedChanges::cancel;
    EXPECT_FALSE(actions().open_dialog());
    EXPECT_EQ(asked_, std::vector<QString>{QStringLiteral("p")});
    EXPECT_EQ(app_.document.size(), 2U);

    answer_ = UnsavedChanges::discard;
    const QSignalSpy opened(&actions(), &ProjectActions::opened);
    EXPECT_TRUE(actions().open_dialog());
    EXPECT_EQ(opened.count(), 1);
    EXPECT_EQ(app_.document.size(), 1U);
    EXPECT_FALSE(window_->isWindowModified());

    // Nothing to ask about now.
    asked_.clear();
    EXPECT_TRUE(actions().open(folder_ / "p.slproj"));
    EXPECT_TRUE(asked_.empty());
}

TEST_F(ShellProject, ClosingTheWindowAsksToo) {
    add_note("a");
    project_.save_as(folder_ / "p");
    add_note("b");

    answer_ = UnsavedChanges::cancel;
    QCloseEvent refused;
    QApplication::sendEvent(window_.get(), &refused);
    EXPECT_FALSE(refused.isAccepted());

    answer_ = UnsavedChanges::save;
    QCloseEvent accepted;
    QApplication::sendEvent(window_.get(), &accepted);
    EXPECT_TRUE(accepted.isAccepted());
    EXPECT_FALSE(project_.is_modified());
}

TEST_F(ShellProject, FailuresGoToTheProblems) {
    chosen_ = folder_ / "not a project";
    fs::create_directories(chosen_);
    EXPECT_FALSE(actions().open_dialog());
    ASSERT_EQ(window_->problems().messages().size(), 1);
    EXPECT_TRUE(window_->problems().messages()[0].contains(QStringLiteral("cannot be opened")))
        << window_->problems().messages()[0].toStdString();
    EXPECT_TRUE(window_->problems().messages()[0].contains(QStringLiteral("not a project folder")));
}

TEST_F(ShellProject, AutosaveFollowsTheSetting) {
    QTimer& timer = actions().autosave_timer();
    EXPECT_TRUE(timer.isActive());
    EXPECT_EQ(timer.interval(), 60 * 1000);
    app_.settings.set(fem::settings::Level::user, fem::settings::keys::autosave_interval, 0);
    EXPECT_FALSE(timer.isActive());
    app_.settings.set(fem::settings::Level::user, fem::settings::keys::autosave_interval, 5);
    EXPECT_EQ(timer.interval(), 5 * 1000);

    // Without a folder there is nothing to save to; with one, a change is saved.
    add_note("a");
    actions().autosave();
    EXPECT_TRUE(project_.is_modified());
    project_.save_as(folder_ / "p");
    app_.settings.set(fem::settings::Level::user, fem::settings::keys::autosave_interval, 1);
    add_note("b");
    EXPECT_TRUE(QTest::qWaitFor([this] { return !project_.is_modified(); }, 5000));
    EXPECT_TRUE(window_->problems().messages().isEmpty());
}

TEST_F(ShellProject, CleanRemovesOldDataAndTellsHowMuch) {
    QAction* clean = actions().clean_action();
    EXPECT_FALSE(clean->isEnabled());  // No folder yet.

    // A mesh large enough to be stored in pieces, saved twice with other coordinates.
    const auto mesh_with = [](double shift) {
        fem::model::NodeArrays nodes;
        for (int i = 0; i < 5000; ++i) {
            nodes.ids.push_back(i + 1);
            nodes.x.push_back(0.001 * i + shift);
            nodes.y.push_back(0.002 * i);
            nodes.z.push_back(0.003 * i);
        }
        return std::make_shared<const fem::model::Mesh>(std::move(nodes),
                                                        std::vector<fem::model::ElementBlock>{});
    };
    std::function<void(fem::model::DocumentEditor&)> edit;
    app_.bus.register_command({.id = "test.edit", .owner = "test.module"},
                              [&edit](fem::command::CommandContext& context) {
                                  edit(context.editor());
                                  return fem::command::CommandResult{};
                              });
    fem::model::ObjectId mesh;
    edit = [&](fem::model::DocumentEditor& editor) {
        mesh = editor.create(fem::model::mesh_object("mesh", mesh_with(0.0)));
    };
    (void)app_.bus.execute("test.edit");
    project_.save_as(folder_ / "p");
    EXPECT_TRUE(clean->isEnabled());
    edit = [&](fem::model::DocumentEditor& editor) { editor.set_data(mesh, mesh_with(1.0)); };
    (void)app_.bus.execute("test.edit");
    project_.save();

    clean->trigger();
    EXPECT_TRUE(window_->prompt().startsWith(QStringLiteral("Project cleaned: ")))
        << window_->prompt().toStdString();
    EXPECT_TRUE(window_->problems().messages().isEmpty());
    EXPECT_TRUE(actions().open(folder_ / "p.slproj"));

    // A damaged block of the saved project: nothing is removed, the reason goes to the problems.
    for (const auto& entry : fs::recursive_directory_iterator(folder_ / "p.slproj" / ".fem")) {
        if (entry.is_regular_file()) {
            ASSERT_TRUE(fem::io::write_file(entry.path(), "garbage"));
        }
    }
    EXPECT_FALSE(actions().clean());
    ASSERT_EQ(window_->problems().messages().size(), 1);
    EXPECT_TRUE(window_->problems().messages()[0].contains(QStringLiteral("cannot be cleaned")))
        << window_->problems().messages()[0].toStdString();

    project_.close();
    EXPECT_FALSE(clean->isEnabled());
}

TEST(ShellProjectWithoutSession, ActionsAreDisabled) {
    fem::shell::testing::Application app;
    MainWindow window(app.workbench());
    EXPECT_FALSE(window.project().save_action()->isEnabled());
    EXPECT_FALSE(window.project().open_action()->isEnabled());
    EXPECT_FALSE(window.project().clean_action()->isEnabled());
    EXPECT_EQ(window.windowTitle(), QStringLiteral("StressLab"));
}

}  // namespace
