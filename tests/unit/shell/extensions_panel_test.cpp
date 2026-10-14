// The module manager and the bar "The model needs the modules…" (task 1.14,
// docs/interface-design.md §6).

#include "fem/shell/extensions_panel.h"

#include "fem/core/extension/manifest.h"
#include "fem/core/extension/profile.h"
#include "fem/core/model/object_id.h"
#include "fem/shell/main_window.h"
#include "fem/shell/required_modules_bar.h"
#include "fem/shell/settings_panel.h"
#include "fem/shell/theme.h"

#include "shell_test_support.h"
#include <DockWidget.h>
#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTest>
#include <QToolBar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace zone = fem::shell::zone;
using fem::extension::ExtensionSource;
using fem::extension::ExtensionState;
using fem::shell::ExtensionsPanel;
using fem::shell::MainWindow;
using fem::shell::RequiredModulesBar;
using fem::shell::testing::find_action;
using fem::shell::testing::q;
using fem::shell::testing::utf8;
using Ids = std::vector<std::string>;

/// The module of the tests: a tree node type, a stage, a command, a setting.
constexpr std::string_view dynamics = R"(
[[contributes.commands]]
id = "dyn.modal"
title = "Modal Analysis"
[[contributes.stages]]
id = "dyn.stage"
title = "Dynamics"
slot = "after:results"
[[contributes.treeNodeTypes]]
type = "damping"
title = "Damping"
[[contributes.propertySchemas]]
type = "damping"
schema = { type = "object" }
[contributes.settings]
"dyn.modes" = { type = "integer", default = 10, title = "Number of modes" }
)";

/// Lets queued rebuilds and deleted dialogs happen.
void settle() {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

/// Writes a module of the user into a folder.
fs::path write_module(const fs::path& folder, std::string_view id, std::string_view version) {
    fs::create_directories(folder);
    std::ofstream out(folder / std::string(fem::extension::manifest_file_name), std::ios::binary);
    out << "[extension]\nid = \"" << id << "\"\nname = \"Mine " << id << "\"\nversion = \""
        << version << "\"\nengine = \"^1.0\"\nkind = \"python\"\nentry = \"mine:activate\"\n"
        << "author = \"" << utf8(u8"Я сам") << "\"\n"
        << "[[contributes.commands]]\nid = \"mine.hello\"\ntitle = \"Hello\"\n";
    return folder;
}

class ShellExtensions : public ::testing::Test {
protected:
    ShellExtensions()
        : folder_(fs::temp_directory_path() / fs::path(u8"менеджер модулей StressLab")),
          library_(folder_ / "profiles") {
        std::error_code ignored;
        fs::remove_all(folder_, ignored);
        fs::create_directories(folder_);
        app_.add("test.dyn", dynamics);
        (void)app_.install("test.need", "", {}, "requires = [\"test.dyn\"]\n");
        app_.add("test.tool",
                 "[[contributes.commands]]\nid = \"tool.run\"\ntitle = \"Run Tool\"\n");
        workbench_.profiles = &library_;
        workbench_.save_modules = [this] { ++saved_; };
        workbench_.dialogs.open_folder = [this](QWidget*, const QString&) {
            return folder_answer_;
        };
        workbench_.dialogs.open_file = [this](QWidget*, const QString&, const QString&) {
            return file_answer_;
        };
        workbench_.dialogs.save_file = [this](QWidget*, const QString&, const QString&,
                                              const QString&) { return file_answer_; };
    }
    ~ShellExtensions() override {
        panel_.reset();
        std::error_code ignored;
        fs::remove_all(folder_, ignored);
    }
    ShellExtensions(const ShellExtensions&) = delete;
    ShellExtensions& operator=(const ShellExtensions&) = delete;
    ShellExtensions(ShellExtensions&&) = delete;
    ShellExtensions& operator=(ShellExtensions&&) = delete;

    void SetUp() override {
        panel_ = std::make_unique<ExtensionsPanel>(workbench_);
        panel_->set_hooks(
            {.layout = [] { return nlohmann::json{{"from", "window"}}; },
             .restore_layout =
                 [this](const nlohmann::json& layout) {
                     restored_.push_back(layout);
                     return true;
                 },
             .show_settings = [this](const std::string& owner) { configured_.push_back(owner); },
             .report = {}});
        panel_->resize(360, 700);
        panel_->show();
        settle();
    }

    /// Answers the dialog of this name that the panel shows; false if there is none.
    bool answer(const QString& name, bool accept) {
        QMessageBox* box = nullptr;
        for (QMessageBox* candidate : panel_->findChildren<QMessageBox*>(name)) {
            if (candidate->isVisible()) {
                box = candidate;
            }
        }
        if (box == nullptr) {
            return false;
        }
        last_question_ = box->text();
        box->findChild<QPushButton*>(accept ? QStringLiteral("accept") : QStringLiteral("cancel"))
            ->click();
        settle();
        return true;
    }

    fem::model::ObjectId create(std::string_view owner, std::string_view type,
                                const std::string& name) {
        return fem::model::ObjectId(static_cast<std::uint64_t>(
            app_.bus
                .execute(
                    "model.create",
                    {{"type", std::string(type)}, {"owner", std::string(owner)}, {"name", name}})
                .value.as_integer()));
    }

    fs::path folder_;
    fem::shell::testing::Application app_;
    fem::extension::ProfileLibrary library_;
    fem::shell::Workbench workbench_ = app_.workbench();
    std::unique_ptr<ExtensionsPanel> panel_;
    int saved_ = 0;
    fs::path folder_answer_;
    fs::path file_answer_;
    std::vector<nlohmann::json> restored_;
    std::vector<std::string> configured_;
    QString last_question_;
};

// --- The lists -----------------------------------------------------------------------------------

TEST_F(ShellExtensions, ListsTheInstalledModulesWithTheirSwitches) {
    EXPECT_EQ(panel_->listed(), (Ids{"test.dyn", "test.need", "test.tool"}));
    ASSERT_NE(panel_->toggle("test.dyn"), nullptr);
    EXPECT_TRUE(panel_->toggle("test.dyn")->isChecked());
    EXPECT_FALSE(panel_->toggle("test.need")->isChecked());
    EXPECT_TRUE(panel_->row_text("test.dyn").contains(QStringLiteral("Module test.dyn  1.0.0")));
    EXPECT_FALSE(panel_->row_text("test.dyn").contains(QStringLiteral("Local")));

    auto* tabs = panel_->findChild<QTabWidget*>(QStringLiteral("extensions.tabs"));
    ASSERT_NE(tabs, nullptr);
    EXPECT_EQ(tabs->count(), 3);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("Installed"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("Catalog"));
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("Updates"));
    // The catalog is empty until there is one.
    EXPECT_TRUE(panel_->findChild<QLabel*>(QStringLiteral("extensions.catalog"))
                    ->text()
                    .contains(QStringLiteral("later version")));
}

TEST_F(ShellExtensions, SearchFiltersByNameAndId) {
    QTest::keyClicks(panel_->search(), QStringLiteral("TOOL"));
    EXPECT_EQ(panel_->listed(), Ids{"test.tool"});
    panel_->search()->setText(QStringLiteral("test.d"));
    EXPECT_EQ(panel_->listed(), Ids{"test.dyn"});
    panel_->search()->setText(QStringLiteral("nothing like it"));
    EXPECT_TRUE(panel_->listed().empty());
    panel_->search()->clear();
    EXPECT_EQ(panel_->listed().size(), 3U);
}

// --- The card ------------------------------------------------------------------------------------

TEST_F(ShellExtensions, TheCardSaysWhatTheModuleAddsToTheInterface) {
    const auto& info =
        app_.manager.install(fs::path(FEM_TEST_EXTENSIONS_DIR) / "full", ExtensionSource::builtin);
    settle();
    panel_->show_card(info.id());
    EXPECT_EQ(panel_->card(), "test.full");
    const QString card = panel_->card_text();
    for (const char* line : {
             "Version 1.2.0-rc.1+build.7",
             "Shipped with the application.",
             "Disabled.",
             "Uses every contribution point",
             "Adds to the interface",
             "Stage bar: ",
             "Solvers: Full Solver",
             "Meshers: Full Mesher",
             "Activity bar: Full",
             "3D view: Mode shapes, Safety factor",
             "Settings: ",
             "Shortcuts: ",
             "Requires",
             "test.base >=1.0: not installed",
         }) {
        EXPECT_TRUE(card.contains(QString::fromUtf8(line))) << line << "\n" << card.toStdString();
    }
    EXPECT_TRUE(card.contains(q(utf8(u8"by Лаборатория прочности")))) << card.toStdString();
    // Every point of docs/interface-design.md §6 the module uses is listed.
    const std::vector<fem::shell::ContributionLine> lines =
        fem::shell::contribution_lines(info, app_.localization);
    QStringList points;
    for (const auto& line : lines) {
        points.push_back(line.point);
    }
    EXPECT_EQ(points,
              QStringList({"Stage bar", "Model tree", "Context toolbar", "Property forms",
                           "Commands", "Menus", "File formats", "Solvers", "Meshers",
                           "Activity bar", "Bottom panel", "3D view", "Settings", "Shortcuts"}));
    // It cannot be uninstalled; it has nothing to configure while disabled.
    EXPECT_EQ(panel_->findChild<QPushButton*>(QStringLiteral("card.uninstall")), nullptr);
    EXPECT_EQ(panel_->findChild<QPushButton*>(QStringLiteral("card.configure")), nullptr);

    panel_->findChild<QPushButton*>(QStringLiteral("card.back"))->click();
    EXPECT_TRUE(panel_->card().empty());
}

TEST_F(ShellExtensions, AModuleThatAddsNothingVisibleSaysSo) {
    const auto& info = app_.install("test.quiet", "");
    EXPECT_TRUE(fem::shell::contribution_lines(info, app_.localization).empty());
    settle();
    panel_->show_card("test.quiet");
    EXPECT_TRUE(panel_->card_text().contains(QStringLiteral("Nothing that shows")));
}

TEST_F(ShellExtensions, ClickingARowOpensTheCardAndConfigureOpensItsSettings) {
    auto* list = panel_->findChild<QListWidget*>(QStringLiteral("extensions.installed"));
    ASSERT_NE(list, nullptr);
    Q_EMIT list->itemClicked(list->item(0));
    EXPECT_EQ(panel_->card(), "test.dyn");
    EXPECT_TRUE(panel_->card_text().contains(QStringLiteral("Stage bar: Dynamics")));
    EXPECT_TRUE(panel_->card_text().contains(QStringLiteral("Settings: Number of modes")));
    auto* configure = panel_->findChild<QPushButton*>(QStringLiteral("card.configure"));
    ASSERT_NE(configure, nullptr);
    configure->click();
    EXPECT_EQ(configured_, Ids{"test.dyn"});
}

// --- Enabling and disabling ----------------------------------------------------------------------

TEST_F(ShellExtensions, DisablingAModuleWithoutObjectsAsksNothing) {
    panel_->toggle("test.tool")->click();
    settle();
    EXPECT_FALSE(answer(QStringLiteral("disableModuleDialog"), true));
    EXPECT_EQ(app_.manager.get("test.tool").state, ExtensionState::disabled);
    EXPECT_FALSE(panel_->toggle("test.tool")->isChecked());
    EXPECT_EQ(saved_, 1);
    panel_->toggle("test.tool")->click();
    settle();
    EXPECT_NE(app_.manager.get("test.tool").state, ExtensionState::disabled);
    EXPECT_EQ(saved_, 2);
}

TEST_F(ShellExtensions, DisablingAModuleWarnsAboutTheNodesItHides) {
    const auto first = create("test.dyn", "damping", "d1");
    (void)create("test.dyn", "damping", "d2");
    (void)create("test.dyn", "damping", "d3");
    panel_->toggle("test.dyn")->click();
    settle();
    // Not yet: the switch shows the module as it is while the question is open.
    EXPECT_TRUE(panel_->toggle("test.dyn")->isChecked());
    ASSERT_TRUE(answer(QStringLiteral("disableModuleDialog"), false));
    EXPECT_TRUE(last_question_.contains(
        QStringLiteral("The open model has 3 nodes of the module “Module test.dyn”.")))
        << last_question_.toStdString();
    EXPECT_TRUE(last_question_.contains(QStringLiteral("kept in the file")));
    EXPECT_NE(app_.manager.get("test.dyn").state, ExtensionState::disabled);
    EXPECT_EQ(saved_, 0);

    panel_->set_enabled("test.dyn", false);
    ASSERT_TRUE(answer(QStringLiteral("disableModuleDialog"), true));
    EXPECT_EQ(app_.manager.get("test.dyn").state, ExtensionState::disabled);
    EXPECT_TRUE(app_.document.is_opaque(first));
    EXPECT_FALSE(panel_->toggle("test.dyn")->isChecked());
    EXPECT_EQ(saved_, 1);

    // Back: the nodes are there again.
    panel_->set_enabled("test.dyn", true);
    EXPECT_FALSE(app_.document.is_opaque(first));
}

TEST_F(ShellExtensions, DisablingAModuleNamesTheModulesThatNeedIt) {
    (void)app_.manager.enable("test.need");
    panel_->set_enabled("test.dyn", false);
    ASSERT_TRUE(answer(QStringLiteral("disableModuleDialog"), true));
    EXPECT_TRUE(last_question_.contains(QStringLiteral("Module test.need")))
        << last_question_.toStdString();
    EXPECT_EQ(app_.manager.get("test.need").state, ExtensionState::disabled);
}

TEST_F(ShellExtensions, AModuleThatCannotBeEnabledSaysWhy) {
    (void)app_.install("test.orphan", "", {}, "requires = [\"test.gone\"]\n");
    settle();
    panel_->set_enabled("test.orphan", true);
    EXPECT_TRUE(panel_->status().contains(QStringLiteral("cannot be enabled")))
        << panel_->status().toStdString();
    EXPECT_TRUE(panel_->status().contains(QStringLiteral("test.gone")));
}

// --- Installing, uninstalling, updating ----------------------------------------------------------

TEST_F(ShellExtensions, InstallsAModuleFromAFolderAfterAWarning) {
    folder_answer_ = write_module(folder_ / fs::path(u8"мой модуль"), "fem.mine", "1.0.0");
    panel_->findChild<QPushButton*>(QStringLiteral("extensions.installFolder"))->click();
    ASSERT_TRUE(answer(QStringLiteral("installModuleDialog"), false));
    EXPECT_TRUE(last_question_.contains(QStringLiteral("not verified")));
    EXPECT_EQ(app_.manager.find("fem.mine"), nullptr);

    panel_->install_from_folder();
    ASSERT_TRUE(answer(QStringLiteral("installModuleDialog"), true));
    const auto* info = app_.manager.find("fem.mine");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->source, ExtensionSource::local);
    EXPECT_TRUE(info->enabled());
    EXPECT_EQ(saved_, 1);
    // Its card is shown, marked as local.
    EXPECT_EQ(panel_->card(), "fem.mine");
    EXPECT_TRUE(panel_->card_text().contains(QStringLiteral("Local: installed from a folder")));
    EXPECT_TRUE(panel_->card_text().contains(q(utf8(u8"by Я сам"))));
    panel_->show_list();
    EXPECT_TRUE(panel_->row_text("fem.mine").contains(QStringLiteral("Local")));
}

TEST_F(ShellExtensions, AFolderWithoutAModuleIsNotInstalled) {
    folder_answer_ = folder_ / "empty";
    fs::create_directories(folder_answer_);
    panel_->install_from_folder();
    EXPECT_FALSE(answer(QStringLiteral("installModuleDialog"), true));
    EXPECT_TRUE(panel_->status().contains(QStringLiteral("it is not a module")));
    // Cancelled: nothing happens.
    folder_answer_.clear();
    panel_->install_from_folder();
    EXPECT_FALSE(answer(QStringLiteral("installModuleDialog"), true));
}

TEST_F(ShellExtensions, UninstallsALocalModule) {
    const fs::path folder = write_module(folder_ / "mine", "fem.mine", "1.0.0");
    (void)app_.manager.install(folder, ExtensionSource::local);
    (void)app_.manager.enable("fem.mine");
    settle();
    panel_->show_card("fem.mine");
    auto* uninstall = panel_->findChild<QPushButton*>(QStringLiteral("card.uninstall"));
    ASSERT_NE(uninstall, nullptr);
    uninstall->click();
    ASSERT_TRUE(answer(QStringLiteral("uninstallModuleDialog"), true));
    EXPECT_TRUE(last_question_.contains(QStringLiteral("stays on the disk")));
    EXPECT_EQ(app_.manager.find("fem.mine"), nullptr);
    EXPECT_TRUE(panel_->card().empty());
    EXPECT_EQ(panel_->listed(), (Ids{"test.dyn", "test.need", "test.tool"}));
    EXPECT_TRUE(fs::exists(folder));
    EXPECT_EQ(saved_, 1);
}

TEST_F(ShellExtensions, UpdatesALocalModuleWhoseFolderChanged) {
    const fs::path folder = write_module(folder_ / "mine", "fem.mine", "1.0.0");
    (void)app_.manager.install(folder, ExtensionSource::local);
    (void)app_.manager.enable("fem.mine");
    settle();
    panel_->show_tab(ExtensionsPanel::Tab::updates);
    EXPECT_TRUE(panel_->offered_updates().empty());

    write_module(folder, "fem.mine", "1.1.0");
    panel_->rebuild();
    EXPECT_EQ(panel_->offered_updates(), Ids{"fem.mine"});
    auto* tabs = panel_->findChild<QTabWidget*>(QStringLiteral("extensions.tabs"));
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("Updates (1)"));
    auto* update = panel_->findChild<QPushButton*>(QStringLiteral("update:fem.mine"));
    ASSERT_NE(update, nullptr);
    update->click();
    settle();
    EXPECT_EQ(app_.manager.get("fem.mine").manifest->version.to_string(), "1.1.0");
    EXPECT_TRUE(app_.manager.get("fem.mine").enabled());
    EXPECT_TRUE(panel_->offered_updates().empty());
    EXPECT_TRUE(panel_->status().contains(QStringLiteral("is updated")));
    EXPECT_EQ(saved_, 1);
}

// --- Profiles ------------------------------------------------------------------------------------

TEST_F(ShellExtensions, SavesAndAppliesProfilesWithTheLayout) {
    panel_->save_profile(utf8(u8"Прочность стали"));
    QComboBox* box = panel_->profile_box();
    ASSERT_EQ(box->count(), 2);  // "Choose a profile" and the profile.
    EXPECT_EQ(box->currentText(), q(utf8(u8"Прочность стали")));
    const auto saved = library_.find(utf8(u8"Прочность стали"));
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->layout, (nlohmann::json{{"from", "window"}}));
    EXPECT_EQ(saved->extensions.size(), 2U);

    (void)app_.manager.disable("test.tool");
    (void)app_.manager.enable("test.need");
    settle();
    box->setCurrentIndex(box->findText(q(utf8(u8"Прочность стали"))));
    panel_->findChild<QPushButton*>(QStringLiteral("profiles.apply"))->click();
    settle();
    EXPECT_TRUE(app_.manager.get("test.tool").enabled());
    EXPECT_FALSE(app_.manager.get("test.need").enabled());
    ASSERT_EQ(restored_.size(), 1U);
    EXPECT_EQ(restored_[0], (nlohmann::json{{"from", "window"}}));
    EXPECT_EQ(saved_, 1);
}

TEST_F(ShellExtensions, ApplyingAProfileWarnsAboutTheNodesItHides) {
    fem::extension::Profile minimal;
    minimal.name = "Minimal";
    minimal.extensions = {{"test.tool", fem::extension::Version(1, 0, 0)}};
    (void)library_.save(minimal);
    (void)create("test.dyn", "damping", "d1");
    panel_->apply_profile("Minimal");
    ASSERT_TRUE(answer(QStringLiteral("applyProfileDialog"), false));
    EXPECT_TRUE(last_question_.contains(QStringLiteral("1 node of the module")))
        << last_question_.toStdString();
    EXPECT_TRUE(app_.manager.get("test.dyn").enabled());
    panel_->apply_profile("Minimal");
    ASSERT_TRUE(answer(QStringLiteral("applyProfileDialog"), true));
    EXPECT_FALSE(app_.manager.get("test.dyn").enabled());
    // An empty layout leaves the window as it is.
    EXPECT_TRUE(restored_.empty());

    // A profile with a module that is not installed says so.
    fem::extension::Profile other;
    other.name = "Other";
    other.extensions = {{"test.absent", fem::extension::Version(1, 0, 0)}};
    (void)library_.save(other);
    panel_->apply_profile("Other");
    EXPECT_TRUE(panel_->status().contains(QStringLiteral("test.absent")));
}

TEST_F(ShellExtensions, ProfilesAreNamedImportedExportedAndDeleted) {
    // "Save Current as Profile…" asks for a name.
    auto* dialog = qobject_cast<QInputDialog*>(panel_->ask_profile_name());
    ASSERT_NE(dialog, nullptr);
    EXPECT_EQ(dialog->objectName(), QStringLiteral("profileNameDialog"));
    dialog->setTextValue(QStringLiteral("  Heat  "));
    dialog->accept();
    settle();
    EXPECT_EQ(library_.names(), Ids{"Heat"});

    file_answer_ = folder_ / fs::path(u8"для коллеги");
    panel_->export_profile("Heat");
    const fs::path exported = folder_ / fs::path(u8"для коллеги.json");
    ASSERT_TRUE(fs::exists(exported));
    EXPECT_EQ(fem::extension::load_profile(exported).name, "Heat");

    panel_->delete_profile("Heat");
    EXPECT_TRUE(library_.names().empty());
    EXPECT_EQ(panel_->profile_box()->count(), 1);

    file_answer_ = exported;
    panel_->import_profile();
    EXPECT_EQ(library_.names(), Ids{"Heat"});
    EXPECT_EQ(panel_->profile_box()->currentText(), QStringLiteral("Heat"));

    // A file that is not a profile is reported.
    file_answer_ = folder_ / "broken.json";
    std::ofstream(file_answer_, std::ios::binary).write("[]", 2);
    panel_->import_profile();
    EXPECT_TRUE(panel_->status().contains(QStringLiteral("cannot be imported")));
}

// --- The bar "The model needs the modules…" ------------------------------------------------------

class ShellRequiredModules : public ShellExtensions {};

TEST_F(ShellRequiredModules, NamesTheModulesTheModelNeedsAndEnablesThem) {
    RequiredModulesBar bar(workbench_);
    bar.check();
    EXPECT_TRUE(bar.isHidden());

    const auto node = create("test.dyn", "damping", "d1");
    (void)app_.manager.disable("test.dyn");
    // Disabling a module does not show the bar: only opening a model does.
    EXPECT_TRUE(bar.isHidden());
    bar.check();
    EXPECT_FALSE(bar.isHidden());
    EXPECT_EQ(bar.modules(), Ids{"test.dyn"});
    EXPECT_EQ(bar.message(), QStringLiteral("The model needs the modules Module test.dyn."));
    auto* enable = bar.findChild<QPushButton*>(QStringLiteral("requiredModules.enable"));
    ASSERT_NE(enable, nullptr);
    EXPECT_FALSE(enable->isHidden());
    enable->click();
    EXPECT_TRUE(app_.manager.get("test.dyn").enabled());
    EXPECT_FALSE(app_.document.is_opaque(node));
    EXPECT_TRUE(bar.isHidden());
    EXPECT_EQ(saved_, 1);
}

TEST_F(ShellRequiredModules, ModulesThatAreNotInstalledAreNamedApart) {
    const fs::path folder = write_module(folder_ / "mine", "fem.mine", "1.0.0");
    (void)app_.manager.install(folder, ExtensionSource::local);
    (void)app_.manager.enable("fem.mine");
    (void)create("fem.mine", "thing", "t");
    app_.manager.uninstall("fem.mine");
    (void)create("test.dyn", "damping", "d1");
    (void)app_.manager.disable("test.dyn");

    RequiredModulesBar bar(workbench_);
    bar.check();
    EXPECT_EQ(bar.modules(), (Ids{"test.dyn", "fem.mine"}));
    EXPECT_EQ(bar.message(), QStringLiteral("The model needs the modules Module test.dyn. These "
                                            "modules are not installed: fem.mine."));
    bar.findChild<QPushButton*>(QStringLiteral("requiredModules.dismiss"))->click();
    EXPECT_TRUE(bar.isHidden());
    EXPECT_FALSE(app_.manager.get("test.dyn").enabled());

    // Only the missing one is left: nothing to enable.
    bar.enable_modules();
    EXPECT_EQ(bar.modules(), Ids{"fem.mine"});
    EXPECT_FALSE(bar.isHidden());
    EXPECT_TRUE(bar.findChild<QPushButton*>(QStringLiteral("requiredModules.enable"))->isHidden());
}

// --- In the window -------------------------------------------------------------------------------

TEST_F(ShellExtensions, TheWindowOpensTheManagerFromTheActivityBar) {
    fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
    MainWindow window(workbench_);
    window.show();
    QApplication::processEvents();
    auto* dock = qobject_cast<ads::CDockWidget*>(window.zone_widget(zone::extensions));
    ASSERT_NE(dock, nullptr);
    EXPECT_EQ(dock->widget(), &window.extensions());
    EXPECT_TRUE(dock->isClosed());
    // Next to the model tree.
    EXPECT_EQ(
        dock->dockAreaWidget(),
        qobject_cast<ads::CDockWidget*>(window.zone_widget(zone::model_tree))->dockAreaWidget());
    auto* activities = qobject_cast<QToolBar*>(window.zone_widget(zone::activity_bar));
    QAction* modules = find_action(activities, QStringLiteral("view.extensions"));
    ASSERT_NE(modules, nullptr);
    EXPECT_EQ(modules->text(), QStringLiteral("Modules"));
    modules->trigger();
    EXPECT_FALSE(dock->isClosed());

    // "Configure" opens the settings at the section of the module.
    window.extensions().show_card("test.dyn");
    window.extensions().findChild<QPushButton*>(QStringLiteral("card.configure"))->click();
    EXPECT_TRUE(window.is_zone_shown(zone::settings));

    // The bar over the 3D view.
    (void)create("test.dyn", "damping", "d1");
    (void)app_.manager.disable("test.dyn");
    window.check_required_modules();
    EXPECT_FALSE(window.required_modules().isHidden());
}

}  // namespace
