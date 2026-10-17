// The history of the project in the window (task 2.6, docs/interface-design.md §12): the panel
// "History" with its graph and "Commit version", a version viewed read-only with the bar over the
// 3D view, switching variants with the question about changes not in the history, the name of
// the variant in colour and in the title, the screen "Compare" and the terms of git.

#include "fem/shell/history_panel.h"

#include "fem/core/command/error.h"
#include "fem/core/model/document.h"
#include "fem/core/settings/settings.h"
#include "fem/shell/comparison_view.h"
#include "fem/shell/history_controller.h"
#include "fem/shell/history_text.h"
#include "fem/shell/keys.h"
#include "fem/shell/main_window.h"
#include "fem/shell/problems.h"
#include "fem/shell/project_actions.h"
#include "fem/shell/version_bar.h"
#include "fem/storage/history.h"
#include "fem/storage/session.h"

#include "shell_test_support.h"
#include <DockWidget.h>
#include <gtest/gtest.h>

#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::model::ObjectId;
using fem::shell::ComparisonView;
using fem::shell::HistoryController;
using fem::shell::HistoryPanel;
using fem::shell::MainWindow;
using fem::shell::PendingChanges;
using fem::shell::testing::find_action;
using fem::shell::testing::q;
using fem::shell::testing::utf8;

/// A module of notes with a size in the units of the model and a key indicator of results.
constexpr const char* notes_module = R"(
[[contributes.treeNodeTypes]]
type = "note"
title = "Note"

[[contributes.propertySchemas]]
type = "note"
version = 1

[contributes.propertySchemas.schema]
type = "object"

[contributes.propertySchemas.schema.properties.size]
type = "number"
title = "Size"
x-quantity = "length"

[[contributes.resultIndicators]]
id = "test.mass"
title = "Mass"
quantity = "mass"

[[contributes.resultIndicators]]
id = "test.cycles"
title = "Cycles"
)";

class ShellHistory : public ::testing::Test {
protected:
    ShellHistory()
        : root_(fs::temp_directory_path() / fs::path(u8"stresslab история окна")),
          folder_(root_ / fs::path(u8"крыло.slproj")),
          project_(app_.bus) {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        fs::create_directories(root_);
        app_.localization.set_language("en");
        app_.add("test.module", notes_module);

        fem::shell::Workbench workbench = app_.workbench();
        workbench.project = &project_;
        workbench.ask_pending = [this](QWidget*, QString& message) {
            asked_.push_back(message);
            if (!message_.isEmpty()) {
                message = message_;
            }
            return pending_;
        };
        workbench.ask_name = [this](QWidget*, const QString&, const QString&, const QString&) {
            return name_;
        };
        window_ = std::make_unique<MainWindow>(workbench);
    }

    ~ShellHistory() override {
        window_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    ObjectId add_note(const char* name, double size) {
        return ObjectId(static_cast<std::uint64_t>(
            app_.bus
                .execute("model.create",
                         {{"type", "note"},
                          {"owner", "test.module"},
                          {"name", name},
                          {"properties", fem::command::Value::Dict{{"size", size}}}})
                .value.as_integer()));
    }
    void set_size(ObjectId note, double size) {
        (void)app_.bus.execute("model.set_property",
                               {{"object", note}, {"key", "size"}, {"value", size}});
    }
    [[nodiscard]] double size(ObjectId note) const {
        return app_.document.get(note).properties().at("size").get<double>();
    }

    /// A saved project with two versions: the note at size 2, then at 3.
    ObjectId two_versions() {
        const ObjectId note = add_note("skin", 2.0);
        project_.save_as(folder_);
        first_ = history().commit("First");
        set_size(note, 3.0);
        second_ = history().commit("Second");
        return note;
    }

    fem::storage::ProjectHistory& history() { return project_.history(); }
    HistoryController& controller() { return window_->history(); }
    HistoryPanel& panel() { return window_->history_panel(); }
    [[nodiscard]] QString text(std::string_view key, const fem::l10n::Args& args = {}) {
        return controller().text(key, args);
    }

    fem::shell::testing::Application app_;
    fs::path root_;
    fs::path folder_;
    fem::storage::ProjectSession project_;
    std::unique_ptr<MainWindow> window_;
    std::string first_;
    std::string second_;
    PendingChanges pending_ = PendingChanges::cancel;
    QString message_;
    std::vector<QString> asked_;
    QString name_;
};

}  // namespace

// --- The panel -----------------------------------------------------------------------------------

TEST_F(ShellHistory, TheActivityOpensThePanelNextToTheTree) {
    QWidget* zone = window_->zone_widget(fem::shell::zone::history);
    ASSERT_NE(zone, nullptr);
    EXPECT_FALSE(window_->is_zone_shown(fem::shell::zone::history));
    QWidget* activities = window_->zone_widget(fem::shell::zone::activity_bar);
    QAction* toggle = window_->zone_action(fem::shell::zone::history);
    ASSERT_NE(toggle, nullptr);
    EXPECT_TRUE(activities->actions().contains(toggle));
    window_->show_history();
    EXPECT_TRUE(window_->is_zone_shown(fem::shell::zone::history));
    EXPECT_EQ(zone->windowTitle(), QStringLiteral("History"));

    // Without a folder there is nothing to keep versions in.
    auto* empty = panel().findChild<QLabel*>(QStringLiteral("history.empty"));
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(empty->text(), text("history.noFolder"));
    EXPECT_FALSE(panel().commit_action()->isEnabled());
    EXPECT_FALSE(panel().start_commit());
    project_.save_as(folder_);
    EXPECT_EQ(empty->text(), text("history.noVersions"));
    EXPECT_TRUE(panel().commit_action()->isEnabled());
}

TEST_F(ShellHistory, CommitVersionListsTheChangesInThePanel) {
    const ObjectId note = add_note("skin", 2.0);
    project_.save_as(folder_);
    // "Commit version" is in the "File" menu with Ctrl/Cmd+Shift+S and opens its form in the panel.
    QAction* commit = panel().commit_action();
    EXPECT_TRUE(find_action(window_->menuBar(), commit->objectName()) != nullptr);
    EXPECT_EQ(commit->shortcut(), fem::shell::key_sequence("Mod+Shift+S"));
    commit->trigger();
    EXPECT_TRUE(window_->is_zone_shown(fem::shell::zone::history));
    ASSERT_TRUE(panel().is_committing());
    auto* message = panel().findChild<QLineEdit*>(QStringLiteral("history.message"));
    ASSERT_NE(message, nullptr);
    // The first version: the message the application gives.
    EXPECT_EQ(message->placeholderText(), text("history.firstVersion"));
    ASSERT_TRUE(panel().finish_commit());
    EXPECT_FALSE(panel().is_committing());
    ASSERT_EQ(history().log().size(), 1U);
    EXPECT_EQ(history().log().front().message, "First version");

    set_size(note, 3.0);
    ASSERT_TRUE(panel().start_commit());
    auto* changes = panel().findChild<QListWidget*>(QStringLiteral("history.changes"));
    ASSERT_NE(changes, nullptr);
    ASSERT_EQ(changes->count(), 1);
    EXPECT_EQ(changes->item(0)->text(), QStringLiteral("Note skin: Size 2 → 3 m"));
    EXPECT_EQ(message->placeholderText(), QStringLiteral("Note skin: Size 2 → 3 m"));
    message->setText(q(utf8(u8"Толщина обшивки 3 мм")));
    panel().findChild<QPushButton*>(QStringLiteral("history.commitOk"))->click();
    EXPECT_EQ(history().log().front().message, utf8(u8"Толщина обшивки 3 мм"));
    EXPECT_EQ(window_->prompt(),
              text("history.committed", {{"message", utf8(u8"Толщина обшивки 3 мм")}}));

    // Nothing to commit: the failure goes to the problems, the form stays.
    ASSERT_TRUE(panel().start_commit());
    EXPECT_FALSE(panel().finish_commit());
    EXPECT_TRUE(panel().is_committing());
    EXPECT_EQ(window_->problems().count(), 1);
}

TEST_F(ShellHistory, TheGraphShowsVersionsVariantsAndLabels) {
    const ObjectId note = two_versions();
    history().tag("v1", first_);
    history().branch(utf8(u8"Композит"), first_);
    controller().refresh();
    QListWidget& graph = panel().graph();
    ASSERT_EQ(graph.count(), 2);
    EXPECT_EQ(panel().version_at(0), second_);
    EXPECT_EQ(panel().version_at(1), first_);
    EXPECT_EQ(graph.item(0)->text(), QStringLiteral("Second"));
    EXPECT_EQ(graph.item(0)->data(HistoryPanel::version_role).toString(), q(second_));
    // A variant without versions of its own shares the lane of the main one.
    EXPECT_EQ(panel().layout().lanes, 1);
    // The second line: author, time and what changed.
    EXPECT_TRUE(graph.item(0)->toolTip().contains(QStringLiteral("Second")));
    EXPECT_EQ(panel().changed_sections(second_), QStringLiteral("Note"));
    EXPECT_EQ(panel().changed_sections(first_), QString());

    // Automatic versions are shown unless filtered out.
    set_size(note, 4.0);
    (void)history().snapshot("Before solving");
    EXPECT_EQ(graph.count(), 3);
    panel().set_show_automatic(false);
    EXPECT_EQ(graph.count(), 3);  // The current version always shows.
    set_size(note, 5.0);
    (void)history().commit("Third");
    EXPECT_EQ(graph.count(), 3);
    panel().set_show_automatic(true);
    EXPECT_EQ(graph.count(), 4);
    panel().set_only_with_labels(true);
    EXPECT_EQ(graph.count(), 2);  // v1 and the current one.
    panel().set_only_with_labels(false);
    panel().set_author(QStringLiteral("nobody"));
    EXPECT_EQ(graph.count(), 1);
    panel().set_author({});
    EXPECT_EQ(graph.count(), 4);
}

// --- Viewing a version ---------------------------------------------------------------------------

TEST_F(ShellHistory, AVersionViewedIsReadOnlyWithABarOverTheView) {
    const ObjectId note = two_versions();
    fem::shell::VersionBar& bar = window_->version_bar();
    EXPECT_TRUE(bar.isHidden());

    ASSERT_TRUE(controller().view(first_));
    EXPECT_TRUE(controller().state().viewing());
    EXPECT_FALSE(bar.isHidden());
    EXPECT_TRUE(bar.message().contains(QStringLiteral("First")));
    EXPECT_EQ(size(note), 2.0);
    // Read-only, the Python console of the window included: it goes through the same bus.
    EXPECT_TRUE(app_.bus.is_read_only());
    EXPECT_THROW(set_size(note, 9.0), fem::command::CommandError);
    EXPECT_EQ(size(note), 2.0);
    EXPECT_FALSE(find_action(window_->menuBar(), QStringLiteral("app.undo"))->isEnabled());
    EXPECT_FALSE(panel().commit_action()->isEnabled());
    // The switcher and the title say what is open.
    const QString viewing = controller().current_title();
    EXPECT_TRUE(viewing.startsWith(QStringLiteral("Version of ")));
    EXPECT_EQ(window_->variant_switcher().text(), viewing);
    EXPECT_TRUE(window_->windowTitle().contains(viewing));

    // [Back to current]
    bar.findChild<QPushButton*>(QStringLiteral("versionBar.back"))->click();
    EXPECT_FALSE(controller().state().viewing());
    EXPECT_EQ(controller().state().branch, std::optional<std::string>("main"));
    EXPECT_FALSE(app_.bus.is_read_only());
    EXPECT_TRUE(bar.isHidden());
    EXPECT_EQ(size(note), 3.0);
}

TEST_F(ShellHistory, ARestoredVersionIsANewVersionOnTop) {
    const ObjectId note = two_versions();
    ASSERT_TRUE(controller().view(first_));
    window_->version_bar().findChild<QPushButton*>(QStringLiteral("versionBar.restore"))->click();
    EXPECT_FALSE(controller().state().viewing());
    EXPECT_EQ(size(note), 2.0);
    const std::vector<fem::storage::VersionInfo> log = history().log();
    ASSERT_EQ(log.size(), 3U);
    EXPECT_EQ(log.front().kind, fem::storage::VersionKind::restore);
    EXPECT_EQ(log.front().restored, first_);
}

TEST_F(ShellHistory, AVariantFromAViewedVersionOpensAtOnce) {
    const ObjectId note = two_versions();
    ASSERT_TRUE(controller().view(first_));
    name_ = q(utf8(u8"Композитная обшивка"));
    window_->version_bar()
        .findChild<QPushButton*>(QStringLiteral("versionBar.newVariant"))
        ->click();
    EXPECT_EQ(controller().state().branch, utf8(u8"Композитная обшивка"));
    EXPECT_FALSE(app_.bus.is_read_only());
    set_size(note, 7.0);
    (void)history().commit("Composite");
    EXPECT_EQ(history().log().front().parents, std::vector<std::string>{first_});
    // The variant has a lane and a colour of its own, in the switcher and in the title.
    EXPECT_EQ(panel().layout().lanes, 2);
    fem::shell::VariantSwitcher& switcher = window_->variant_switcher();
    EXPECT_EQ(switcher.text(), q(utf8(u8"Композитная обшивка")));
    EXPECT_TRUE(switcher.styleSheet().contains(
        fem::shell::variant_color(utf8(u8"Композитная обшивка")).name()));
    EXPECT_NE(fem::shell::variant_color(utf8(u8"Композитная обшивка")),
              fem::shell::variant_color("main"));
    EXPECT_EQ(window_->windowTitle(), q(utf8(u8"крыло — Композитная обшивка[*] — StressLab")));
    // Cancelled: nothing happens.
    name_.clear();
    EXPECT_FALSE(controller().make_variant(first_));
}

// --- Switching variants --------------------------------------------------------------------------

TEST_F(ShellHistory, SwitchingAsksToCommitOrPutAsideTheChanges) {
    const ObjectId note = two_versions();
    history().branch("B", first_);
    controller().refresh();
    set_size(note, 5.0);

    // Cancelled: the model stays as it is.
    pending_ = PendingChanges::cancel;
    EXPECT_FALSE(controller().switch_to("B"));
    ASSERT_EQ(asked_.size(), 1U);
    EXPECT_EQ(asked_.back(), QStringLiteral("Note skin: Size 3 → 5 m"));
    EXPECT_EQ(size(note), 5.0);
    EXPECT_EQ(controller().state().branch, std::optional<std::string>("main"));

    // Put aside: the changes wait in the panel.
    pending_ = PendingChanges::stash;
    ASSERT_TRUE(controller().switch_to("B"));
    EXPECT_EQ(size(note), 2.0);
    EXPECT_EQ(history().stashes().size(), 1U);
    EXPECT_FALSE(panel().findChild<QWidget*>(QStringLiteral("history.stashes"))->isHidden());

    // Committed with the message the user gave.
    set_size(note, 8.0);
    pending_ = PendingChanges::commit;
    message_ = QStringLiteral("Eight on B");
    ASSERT_TRUE(controller().switch_to("main"));
    EXPECT_EQ(history().log("B").front().message, "Eight on B");
    EXPECT_EQ(size(note), 3.0);

    // Back on main, the changes put aside come back.
    panel().findChild<QPushButton*>(QStringLiteral("history.stashPop"))->click();
    EXPECT_EQ(size(note), 5.0);
    EXPECT_TRUE(history().stashes().empty());
    EXPECT_TRUE(panel().findChild<QWidget*>(QStringLiteral("history.stashes"))->isHidden());
}

TEST_F(ShellHistory, TheSwitcherListsTheVariants) {
    (void)two_versions();
    history().branch(utf8(u8"Композит"), first_);
    controller().refresh();
    fem::shell::VariantSwitcher& switcher = window_->variant_switcher();
    // On the stage bar while the project has a folder.
    EXPECT_TRUE(find_action(window_->zone_widget(fem::shell::zone::stage_bar),
                            QStringLiteral("stageBar.variant"))
                    ->isVisible());
    EXPECT_EQ(switcher.text(), QStringLiteral("Main variant"));
    switcher.rebuild_menu();
    QStringList variants;
    QAction* composite = nullptr;
    for (QAction* action : switcher.variant_menu().actions()) {
        if (action->objectName() == QStringLiteral("variantSwitcher.variant")) {
            variants.push_back(action->text());
            if (action->data().toString() == q(utf8(u8"Композит"))) {
                composite = action;
            }
        }
    }
    EXPECT_EQ(variants, (QStringList{QStringLiteral("Main variant"), q(utf8(u8"Композит"))}));
    ASSERT_NE(composite, nullptr);
    composite->trigger();
    EXPECT_EQ(controller().state().branch, utf8(u8"Композит"));
    // The selector of the panel follows.
    auto* selector = panel().findChild<QComboBox*>(QStringLiteral("history.variants"));
    ASSERT_NE(selector, nullptr);
    EXPECT_EQ(selector->currentData().toString(), q(utf8(u8"Композит")));
    selector->setCurrentIndex(selector->findData(QStringLiteral("main")));
    Q_EMIT selector->activated(selector->currentIndex());
    EXPECT_EQ(controller().state().branch, std::optional<std::string>("main"));
}

TEST_F(ShellHistory, TheContextMenuOfAVersion) {
    (void)two_versions();
    std::unique_ptr<QMenu> menu(panel().version_menu(first_, window_.get()));
    QStringList names;
    for (const QAction* action : menu->actions()) {
        if (!action->isSeparator()) {
            names.push_back(action->objectName());
        }
    }
    EXPECT_EQ(names, (QStringList{"history.menu.view", "history.menu.newVariant",
                                  "history.menu.label", "history.menu.restore",
                                  "history.menu.compareCurrent", "history.menu.mark"}));
    name_ = QStringLiteral("v1");
    find_action(menu.get(), QStringLiteral("history.menu.label"))->trigger();
    EXPECT_EQ(history().tags().at("v1"), first_);
    // The current version cannot be viewed or restored: it is open.
    std::unique_ptr<QMenu> current(panel().version_menu(second_, window_.get()));
    EXPECT_FALSE(find_action(current.get(), QStringLiteral("history.menu.view"))->isEnabled());
    EXPECT_FALSE(find_action(current.get(), QStringLiteral("history.menu.restore"))->isEnabled());
    // Marked for comparison, another version compares with it.
    panel().mark_for_comparison(first_);
    std::unique_ptr<QMenu> marked(panel().version_menu(second_, window_.get()));
    QAction* compare = find_action(marked.get(), QStringLiteral("history.menu.compareMarked"));
    ASSERT_NE(compare, nullptr);
    compare->trigger();
    EXPECT_TRUE(window_->is_comparing());
    EXPECT_EQ(window_->comparison().before(), q(first_));
    EXPECT_EQ(window_->comparison().after(), q(second_));
}

// --- Compare -------------------------------------------------------------------------------------

TEST_F(ShellHistory, TheComparisonShowsTheDifferencesBySectionsOfTheTree) {
    const ObjectId note = two_versions();
    const ObjectId gust = add_note("gust", 1.0);
    window_->show_comparison(q(first_), {});
    ASSERT_TRUE(window_->is_comparing());
    ComparisonView& comparison = window_->comparison();
    EXPECT_EQ(comparison.before(), q(first_));
    EXPECT_TRUE(comparison.after().isEmpty());
    QTreeWidget& tree = comparison.differences();
    ASSERT_EQ(tree.topLevelItemCount(), 1);
    QTreeWidgetItem* section = tree.topLevelItem(0);
    EXPECT_EQ(section->text(0), QStringLiteral("Note (2)"));
    ASSERT_EQ(section->childCount(), 2);
    QTreeWidgetItem* skin = section->child(0);
    EXPECT_EQ(skin->text(0), QStringLiteral("Note skin"));
    ASSERT_EQ(skin->childCount(), 1);
    EXPECT_EQ(skin->child(0)->text(0), QStringLiteral("Size"));
    EXPECT_EQ(skin->child(0)->text(1), QStringLiteral("2 m"));
    EXPECT_EQ(skin->child(0)->text(2), QStringLiteral("3 m"));
    QTreeWidgetItem* added = section->child(1);
    EXPECT_EQ(added->text(0), QStringLiteral("Note gust"));
    EXPECT_EQ(added->text(2), QStringLiteral("Added"));

    // B is the current model: checked objects come back as they are in A, one step of undo.
    auto* restore = comparison.findChild<QPushButton*>(QStringLiteral("comparison.restore"));
    ASSERT_NE(restore, nullptr);
    EXPECT_FALSE(restore->isHidden());
    EXPECT_FALSE(restore->isEnabled());
    ASSERT_TRUE(comparison.set_checked(note, true));
    EXPECT_TRUE(restore->isEnabled());
    restore->click();
    EXPECT_EQ(size(note), 2.0);
    EXPECT_TRUE(app_.document.contains(gust));
    ASSERT_TRUE(app_.bus.undo());
    EXPECT_EQ(size(note), 3.0);

    // A is the earlier one: the current model goes to B.
    comparison.compare({}, q(second_));
    EXPECT_EQ(comparison.before(), q(second_));
    EXPECT_TRUE(comparison.after().isEmpty());
    // Two versions: nothing to restore into.
    comparison.compare(q(first_), q(second_));
    EXPECT_TRUE(restore->isHidden());
    comparison.findChild<QToolButton*>(QStringLiteral("comparison.close"))->click();
    EXPECT_FALSE(window_->is_comparing());
}

TEST_F(ShellHistory, TheComparisonShowsTheKeyIndicatorsOfResults) {
    const ObjectId note = two_versions();
    fem::storage::ResultCache& results = project_.results();
    const fem::storage::SolverInfo solver{"calculix", "2.22"};
    const fs::path out = root_ / "out";
    fs::create_directories(out);
    std::ofstream(out / "a.frd") << "a";
    (void)results.put(results.key_at(first_, solver), first_, solver, {}, out,
                      {{"test.mass", 400.0}, {"test.cycles", 1000.0}, {"other.max", 7.0}});
    (void)results.put(results.key_at(second_, solver), second_, solver, {}, out,
                      {{"test.mass", 500.0}, {"test.cycles", 0.0}});
    window_->show_comparison(q(first_), q(second_));
    QTableWidget& table = window_->comparison().indicators();
    ASSERT_FALSE(table.isHidden());
    ASSERT_EQ(table.rowCount(), 3);
    // The indicators of modules in their order, with the units of the model (SI by default); the
    // others by id.
    EXPECT_EQ(table.item(0, 0)->text(), QStringLiteral("Mass"));
    EXPECT_EQ(table.item(0, 1)->text(), QStringLiteral("400 kg"));
    EXPECT_EQ(table.item(0, 2)->text(), QStringLiteral("500 kg"));
    EXPECT_EQ(table.item(0, 3)->text(), QStringLiteral("+25 %"));
    EXPECT_EQ(table.item(1, 0)->text(), QStringLiteral("Cycles"));
    EXPECT_EQ(table.item(1, 3)->text(), QStringLiteral("-100 %"));
    EXPECT_EQ(table.item(2, 0)->text(), QStringLiteral("other.max"));
    EXPECT_EQ(table.item(2, 2)->text(), QStringLiteral("—"));
    EXPECT_EQ(table.item(2, 3)->text(), QStringLiteral("—"));
    // The graph says which versions have results.
    controller().refresh();
    EXPECT_FALSE(controller().results_of(first_).empty());
    EXPECT_FALSE(controller().results_outdated("main"));

    // The current model with changes was not computed: no indicators for it.
    set_size(note, 9.0);
    window_->show_comparison(q(first_), {});
    EXPECT_EQ(window_->comparison().indicators().item(0, 2)->text(), QStringLiteral("—"));
    (void)history().commit("Nine");
    controller().refresh();
    EXPECT_TRUE(controller().results_outdated("main"));
}

// --- Words ---------------------------------------------------------------------------------------

TEST_F(ShellHistory, TheTermsOfGitAreASetting) {
    (void)two_versions();
    auto* commit = panel().findChild<QToolButton*>(QStringLiteral("history.commit"));
    ASSERT_NE(commit, nullptr);
    EXPECT_EQ(commit->text(), QStringLiteral("Commit version"));
    EXPECT_EQ(controller().variant_title("main"), QStringLiteral("Main variant"));
    app_.settings.set(fem::settings::Level::user, fem::settings::keys::git_terms, true);
    EXPECT_EQ(commit->text(), QStringLiteral("Commit"));
    EXPECT_EQ(controller().variant_title("main"), QStringLiteral("main"));
    EXPECT_EQ(window_->variant_switcher().text(), QStringLiteral("main"));
    // Strings without a git form stay as they are.
    EXPECT_EQ(controller().text("history.compareButton"), QStringLiteral("Compare"));
}

TEST_F(ShellHistory, RussianWords) {
    (void)two_versions();
    app_.localization.set_language("ru");
    EXPECT_EQ(controller().variant_title("main"), q(utf8(u8"Основной вариант")));
    EXPECT_EQ(window_->variant_switcher().text(), q(utf8(u8"Основной вариант")));
    EXPECT_EQ(panel().findChild<QToolButton*>(QStringLiteral("history.commit"))->text(),
              q(utf8(u8"Зафиксировать")));
    EXPECT_EQ(window_->zone_widget(fem::shell::zone::history)->windowTitle(), q(utf8(u8"История")));
}
