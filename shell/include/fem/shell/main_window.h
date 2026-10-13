#pragma once

#include "fem/core/l10n/localization.h"
#include "fem/core/signal.h"
#include "fem/shell/workspaces.h"

#include <QMainWindow>
#include <QString>
#include <string_view>
#include <vector>

class QAction;
class QActionGroup;
class QCloseEvent;
class QLabel;
class QMenu;
class QToolBar;

namespace ads {
class CDockManager;
class CDockWidget;
}  // namespace ads

namespace fem::shell {

class AboutDialog;

/// Ids of the zones of the window (docs/interface-design.md §4): the object names of their
/// widgets. Saved layouts refer to the zones by them, so they never change.
namespace zone {
inline constexpr std::string_view stage_bar = "stageBar";
inline constexpr std::string_view activity_bar = "activityBar";
inline constexpr std::string_view viewport = "viewport";
inline constexpr std::string_view model_tree = "modelTree";
inline constexpr std::string_view properties = "properties";
inline constexpr std::string_view prompt_line = "promptLine";
// The bottom panel: one tab each.
inline constexpr std::string_view journal = "journal";
inline constexpr std::string_view python_console = "pythonConsole";
inline constexpr std::string_view jobs = "jobs";
inline constexpr std::string_view problems = "problems";
}  // namespace zone

/// The window of the application (task 1.11): the frame of zones that modules fill through the
/// registry of contributions (task 1.12). Nothing in it knows of finite elements.
///
///   menu (the system menu bar on macOS)
///   stage bar
///   activity bar | model tree | 3D view | properties
///   prompt line
///   bottom panel: Journal | Python Console | Jobs | Problems
///
/// The stage bar and the activity bar are tool bars, the 3D view is the central area, the rest are
/// panels of the Qt Advanced Docking System. Every zone but the 3D view can be hidden from the
/// "View" menu; panels are dragged by their title bars and tabs (the prompt line has none: like a
/// status bar it is only hidden). Layouts are saved as named workspaces in a `WorkspaceStore`;
/// the window reopens with the layout it had when it was closed. Texts follow the language of the
/// localization.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /// The localization (with the shell catalogs added) and the store outlive the window.
    MainWindow(l10n::Localization& localization, WorkspaceStore& workspaces,
               QWidget* parent = nullptr);
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
    void rebuild_workspace_menu();
    void ask_workspace_name();
    void retranslate();
    [[nodiscard]] const Zone* find_zone(std::string_view id) const;
    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;

    l10n::Localization& localization_;
    WorkspaceStore& workspaces_;
    ads::CDockManager* docks_ = nullptr;
    std::vector<Zone> zones_;
    QLabel* prompt_ = nullptr;
    QString prompt_text_;
    Layout default_layout_;

    QMenu* file_menu_ = nullptr;
    QMenu* view_menu_ = nullptr;
    QMenu* workspace_menu_ = nullptr;
    QMenu* help_menu_ = nullptr;
    QAction* quit_ = nullptr;
    QAction* full_screen_ = nullptr;
    QAction* about_ = nullptr;
    QActionGroup* workspace_group_ = nullptr;

    Subscription language_;
};

}  // namespace fem::shell
