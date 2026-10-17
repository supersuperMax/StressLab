#pragma once

#include "fem/core/l10n/localization.h"
#include "fem/core/signal.h"
#include "fem/shell/workbench.h"
#include "fem/shell/workspaces.h"

#include <QMainWindow>
#include <QPoint>
#include <QString>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class QAction;
class QActionGroup;
class QCloseEvent;
class QLabel;
class QMenu;
class QStackedWidget;
class QToolBar;

namespace ads {
class CDockManager;
class CDockWidget;
}  // namespace ads

namespace fem::shell {

class AboutDialog;
class CommandPalette;
class Commands;
class ComparisonView;
class ContributionUi;
class ExtensionsPanel;
class HistoryController;
class HistoryPanel;
class JournalPanel;
class ModelTreePanel;
class ProblemsPanel;
class ProjectActions;
class PropertyPanel;
class PythonConsolePanel;
class RequiredModulesBar;
class SettingsPanel;
class VariantSwitcher;
class VersionBar;

/// Ids of the zones of the window (docs/interface-design.md §4): the object names of their
/// widgets. Saved layouts refer to the zones by them, so they never change.
namespace zone {
inline constexpr std::string_view stage_bar = "stageBar";
inline constexpr std::string_view activity_bar = "activityBar";
inline constexpr std::string_view viewport = "viewport";
inline constexpr std::string_view model_tree = "modelTree";
inline constexpr std::string_view properties = "properties";
/// The settings of the application and of modules: a tab next to the properties, closed until
/// opened from the menu.
inline constexpr std::string_view settings = "settings";
/// The module manager: a tab next to the model tree, opened by the activity "Modules".
inline constexpr std::string_view extensions = "extensions";
/// The history of the project (task 2.6): a tab next to the model tree, opened by the activity
/// "History".
inline constexpr std::string_view history = "history";
inline constexpr std::string_view prompt_line = "promptLine";
// The bottom panel: one tab each.
inline constexpr std::string_view journal = "journal";
inline constexpr std::string_view python_console = "pythonConsole";
inline constexpr std::string_view jobs = "jobs";
inline constexpr std::string_view problems = "problems";
}  // namespace zone

/// The window of the application (task 1.11): the frame of zones that modules fill through the
/// registry of contributions (task 1.12, `ContributionUi`). Nothing in it knows of finite
/// elements.
///
///   menu (the system menu bar on macOS)
///   stage bar
///   activity bar | model tree | 3D view | properties (settings)
///   prompt line
///   bottom panel: Journal | Python Console | Jobs | Problems
///
/// The stage bar and the activity bar are tool bars, the 3D view is the central area, the rest are
/// panels of the Qt Advanced Docking System. Every zone but the 3D view can be hidden from the
/// "View" menu; panels are dragged by their title bars and tabs (the prompt line has none: like a
/// status bar it is only hidden). Layouts are saved as named workspaces in a `WorkspaceStore`;
/// the window reopens with the layout it had when it was closed. Texts follow the language of the
/// localization.
///
/// Besides the zones the shell has its own few items: "Edit" with undo and redo of the Command
/// Bus, the command palette (Ctrl/Cmd+Shift+P) in "View", "Settings" (Ctrl/Cmd+,; in the
/// application menu on macOS), the context toolbar at the top of the 3D view, and the "Problems"
/// tab that lists modules whose code crashed or could not start.
///
/// The model tree and the properties are the panels of task 1.13 (docs/adr/0014-tree-and-forms.md):
/// selecting an object in the tree shows its form in the properties.
///
/// Task 1.14 (docs/adr/0015-console-journal-extensions.md) adds the "Journal" and "Python Console"
/// tabs, the module manager and the bar "The model needs the modules…" over the 3D view. The
/// activity bar starts with the activities of the shell: Model, Modules and Settings.
///
/// Task 2.1 (docs/adr/0017-project-folder.md) adds opening and saving the project folder to the
/// "File" menu (`ProjectActions`); the window asks about unsaved changes before it closes.
///
/// Task 2.6 (docs/adr/0022-history-ui.md) adds the history of the project: the panel "History"
/// next to the model tree, "Commit version" (Ctrl/Cmd+Shift+S) in the "File" menu, the bar over
/// the 3D view while a version is viewed, the name of the current variant in its colour at the end
/// of the stage bar and in the title, and the screen "Compare" in place of the 3D view.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /// Everything in `workbench` outlives the window.
    explicit MainWindow(Workbench workbench, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// The widget of a zone (`zone::...`): a tool bar, the 3D view or a dock panel.
    [[nodiscard]] QWidget* zone_widget(std::string_view id) const;
    /// Whether a zone is shown (a closed panel or a hidden bar is not).
    [[nodiscard]] bool is_zone_shown(std::string_view id) const;
    /// The action of the "View" menu that shows and hides a zone; null for the 3D view.
    [[nodiscard]] QAction* zone_action(std::string_view id) const;
    [[nodiscard]] ads::CDockManager* dock_manager() const noexcept { return docks_; }

    /// Shows a hint in the prompt line; empty: the "Ready" text.
    void set_prompt(const QString& text);
    [[nodiscard]] QString prompt() const;

    /// The current layout.
    [[nodiscard]] Layout layout() const;
    /// Puts the panels and bars where a layout has them; false if it cannot be read.
    bool restore_layout(const Layout& layout);
    /// The layout the window was built with.
    [[nodiscard]] const Layout& default_layout() const noexcept { return default_layout_; }

    /// Saves the current layout as a workspace (replacing one of that name) and makes it
    /// current; the store is written.
    void save_workspace(const QString& name);
    /// Opens a workspace; false if there is none of that name or it cannot be read.
    bool open_workspace(const QString& name);
    /// Deletes a workspace; the store is written.
    bool delete_workspace(const QString& name);
    /// Puts everything back where the window was built with it; no workspace is current.
    void reset_layout();
    /// The name of the current workspace; empty: the default layout.
    [[nodiscard]] QString current_workspace() const { return workspaces_.current(); }

    /// Opens "About" over the window, without blocking it; the dialog deletes itself when closed.
    AboutDialog* show_about();

    /// What modules contribute to the window.
    [[nodiscard]] ContributionUi& contributions() const noexcept { return *contributions_; }
    /// The command palette (hidden until opened).
    [[nodiscard]] CommandPalette& palette() const noexcept { return *palette_; }
    /// Opens the command palette.
    void show_palette();
    /// The list of the "Problems" tab.
    [[nodiscard]] ProblemsPanel& problems() const noexcept { return *problems_; }
    /// The context toolbar at the top of the 3D view.
    [[nodiscard]] QToolBar* context_toolbar() const noexcept { return context_toolbar_; }
    /// The model tree, the properties and the settings.
    [[nodiscard]] ModelTreePanel& model_tree() const noexcept { return *tree_; }
    [[nodiscard]] PropertyPanel& properties() const noexcept { return *properties_; }
    [[nodiscard]] SettingsPanel& settings() const noexcept { return *settings_; }
    /// Opens the settings and brings them to the front.
    void show_settings();
    /// Opens the settings at the section of an owner (a module).
    void show_settings_of(const std::string& owner);
    /// The tabs of the bottom panel and the module manager of task 1.14.
    [[nodiscard]] JournalPanel& journal() const noexcept { return *journal_; }
    [[nodiscard]] PythonConsolePanel& console() const noexcept { return *console_; }
    [[nodiscard]] ExtensionsPanel& extensions() const noexcept { return *extensions_; }
    /// Opens the module manager and brings it to the front.
    void show_extensions();
    /// The bar "The model needs the modules…".
    [[nodiscard]] RequiredModulesBar& required_modules() const noexcept { return *required_; }
    /// Shows the bar if the model needs modules that are not enabled: called at the start of the
    /// window and when a project is opened.
    void check_required_modules();
    /// Open, save and autosave of the project folder (task 2.1), in the "File" menu.
    [[nodiscard]] ProjectActions& project() const noexcept { return *project_; }
    /// The action "Save journal as script…" of the "File" menu.
    [[nodiscard]] QAction* save_journal_action() const noexcept;
    /// The history of the project in the window and what shows it (task 2.6).
    [[nodiscard]] HistoryController& history() const noexcept { return *history_; }
    [[nodiscard]] HistoryPanel& history_panel() const noexcept { return *history_panel_; }
    [[nodiscard]] VersionBar& version_bar() const noexcept { return *version_bar_; }
    [[nodiscard]] VariantSwitcher& variant_switcher() const noexcept { return *variant_switcher_; }
    [[nodiscard]] ComparisonView& comparison() const noexcept { return *comparison_; }
    /// Opens the panel "History" and brings it to the front.
    void show_history();
    /// Shows the screen "Compare" in place of the 3D view, comparing two states (versions by id,
    /// variants or labels; empty is the current model).
    void show_comparison(const QString& before, const QString& after);
    /// Shows the 3D view again.
    void close_comparison();
    [[nodiscard]] bool is_comparing() const;
    /// The commands of modules as the window runs them.
    [[nodiscard]] Commands& commands() const noexcept { return *commands_; }
    /// Opens the context menu of a location ("tree", "viewport") of modules at a point of a
    /// widget; nullptr if no item shows there now. The menu deletes itself when closed.
    QMenu* show_context_menu(std::string_view location, QWidget* widget, const QPoint& position);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Zone {
        std::string_view id;
        std::string_view title_key;
        QWidget* widget = nullptr;
        ads::CDockWidget* dock = nullptr;  ///< Panels only.
        QToolBar* bar = nullptr;           ///< Bars only.
        QLabel* placeholder = nullptr;
    };

    void build_zones();
    void apply_default_sizes();
    void build_menus();
    void build_contributions();
    void update_history();
    void on_extension_event(const extension::ExtensionEvent& event);
    void rebuild_workspace_menu();
    void ask_workspace_name();
    void retranslate();
    [[nodiscard]] const Zone* find_zone(std::string_view id) const;
    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;

    Workbench workbench_;
    l10n::Localization& localization_;
    WorkspaceStore& workspaces_;
    ads::CDockManager* docks_ = nullptr;
    std::vector<Zone> zones_;
    QLabel* prompt_ = nullptr;
    QString prompt_text_;
    Layout default_layout_;

    QMenu* file_menu_ = nullptr;
    QMenu* edit_menu_ = nullptr;
    QMenu* view_menu_ = nullptr;
    QMenu* workspace_menu_ = nullptr;
    QMenu* help_menu_ = nullptr;
    QAction* quit_ = nullptr;
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    QAction* palette_action_ = nullptr;
    QAction* settings_action_ = nullptr;
    QAction* full_screen_ = nullptr;
    QAction* about_ = nullptr;
    QActionGroup* workspace_group_ = nullptr;

    QToolBar* context_toolbar_ = nullptr;
    ProblemsPanel* problems_ = nullptr;
    ModelTreePanel* tree_ = nullptr;
    PropertyPanel* properties_ = nullptr;
    SettingsPanel* settings_ = nullptr;
    JournalPanel* journal_ = nullptr;
    PythonConsolePanel* console_ = nullptr;
    ExtensionsPanel* extensions_ = nullptr;
    RequiredModulesBar* required_ = nullptr;
    ProjectActions* project_ = nullptr;
    HistoryController* history_ = nullptr;
    HistoryPanel* history_panel_ = nullptr;
    VersionBar* version_bar_ = nullptr;
    VariantSwitcher* variant_switcher_ = nullptr;
    ComparisonView* comparison_ = nullptr;
    QStackedWidget* viewport_stack_ = nullptr;
    std::unique_ptr<Commands> commands_;
    std::unique_ptr<ContributionUi> contributions_;
    CommandPalette* palette_ = nullptr;

    Subscription language_;
    Subscription extension_events_;
    Subscription model_changed_;
    std::vector<Subscription> history_watches_;
};

}  // namespace fem::shell
