#pragma once

#include "fem/core/context/when.h"
#include "fem/core/signal.h"
#include "fem/shell/commands.h"
#include "fem/shell/menu_order.h"
#include "fem/shell/workbench.h"

#include <QKeySequence>
#include <QObject>
#include <QPointer>
#include <QString>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QAction;
class QActionGroup;
class QEvent;
class QMainWindow;
class QMenu;
class QMenuBar;
class QToolBar;
class QToolButton;
class QWidget;

namespace ads {
class CDockManager;
class CDockWidget;
}  // namespace ads

namespace fem::shell {

/// Where the window puts what modules contribute.
struct ContributionHosts {
    /// Shortcuts of modules are actions of the window.
    QMainWindow* window = nullptr;
    QMenuBar* menu_bar = nullptr;
    /// The menus of the shell by id ("file", "edit", "view", "help"): items of modules at these
    /// locations go to their end. Menus of modules stand before "help".
    std::map<std::string, QMenu*, std::less<>> shell_menus;
    QToolBar* stage_bar = nullptr;
    QToolBar* activity_bar = nullptr;
    /// The context toolbar, at the top of the 3D view.
    QToolBar* context_toolbar = nullptr;
    ads::CDockManager* docks = nullptr;
    /// Side panels of activities open as tabs of the area of this panel (the model tree).
    ads::CDockWidget* side_panel = nullptr;
    /// Tabs of modules join the area of this panel (the journal).
    ads::CDockWidget* bottom_panel = nullptr;
};

/// Builds the zones of the window from the registry of contributions (docs/architecture.md
/// §4.2–4.3, docs/interface-design.md §6, docs/adr/0013-shell-contributions.md): menus, the context
/// toolbar, context menus, the stage bar, the activity bar, tabs of the bottom panel and
/// shortcuts.
///
/// What it builds is rebuilt when a module is enabled or disabled and when the language changes;
/// what shows and what is available follows the context keys at once (`when` of each place,
/// `enablement` of each command), without a restart. Panels of modules that stay enabled are kept
/// as they are, so a rebuild does not move them.
///
/// Object names, for layouts and tests: menus of modules "menu.<location>"; command items the
/// command id; stages "stage.<id>"; activities "activity.<id>" with the panel "activity:<id>";
/// tabs "tab:<id>"; shortcuts "key.<sequence>"; the "⋯" button "contextToolbar.more".
class ContributionUi : public QObject {
    Q_OBJECT

public:
    /// The most commands the context toolbar shows; the others go to its "⋯" menu.
    static constexpr int max_toolbar_commands = 12;

    ContributionUi(Workbench& workbench, Commands& commands, ContributionHosts hosts,
                   QObject* parent = nullptr);
    ContributionUi(const ContributionUi&) = delete;
    ContributionUi& operator=(const ContributionUi&) = delete;
    ContributionUi(ContributionUi&&) = delete;
    ContributionUi& operator=(ContributionUi&&) = delete;
    ~ContributionUi() override;

    /// Builds everything again from the registry.
    void rebuild();
    /// Shows, hides, enables and checks what was built, for the current context keys.
    void refresh();
    /// Opens and closes the tabs of modules by their `when` again: a restored layout puts them
    /// where it had them.
    void apply_tab_conditions();

    /// A new menu of the context-menu location (`contextMenus`, e.g. "tree", "viewport") with the
    /// items that show now; nullptr if none does. The caller owns it.
    [[nodiscard]] QMenu* context_menu(std::string_view location, QWidget* parent = nullptr) const;

    /// The "⋯" button of the context toolbar.
    [[nodiscard]] QToolButton* overflow_button() const noexcept { return overflow_button_; }
    /// The menu of the "⋯" button.
    [[nodiscard]] QMenu* overflow_menu() const noexcept { return overflow_menu_; }
    /// Whether the "⋯" button is on the context toolbar (it is when something is under it). The
    /// toolbar itself may still put it into its extension menu when the 3D view is narrow.
    [[nodiscard]] bool overflow_shown() const;
    /// The panels of modules: activities and tabs.
    [[nodiscard]] std::vector<ads::CDockWidget*> module_panels() const;

Q_SIGNALS:
    /// Everything was built again.
    void rebuilt();

protected:
    /// Refreshes before a tooltip of the context toolbar shows its reason.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// A command placed in a menu or on a toolbar.
    struct Placement {
        std::string command;
        std::optional<context::WhenExpression> when;
        MenuGroup group;
        QString title;
        QPointer<QAction> action;
        QPointer<QAction> overflow;  ///< Context toolbar only: its item in the "⋯" menu.
    };
    /// A menu built for modules, or a menu of the shell they add items to.
    struct BuiltMenu {
        std::string path;
        QPointer<QMenu> menu;
        bool shell = false;  ///< A menu of the shell: never hidden, not deleted.
        std::vector<Placement> items;
        std::vector<BuiltMenu*> children;
    };
    struct Stage {
        std::string id;
        std::optional<context::WhenExpression> when;
        QPointer<QAction> action;
    };
    struct Tab {
        std::optional<context::WhenExpression> when;
        std::optional<bool> shown;  ///< What `when` gave last; nothing before the first time.
    };
    struct Binding {
        std::string command;
        std::optional<context::WhenExpression> when;
    };
    struct Shortcut {
        QKeySequence sequence;
        std::vector<Binding> bindings;
        QPointer<QAction> action;
    };

    void clear();
    void build_menus();
    void build_context_toolbar();
    void build_stages();
    void build_activities();
    void build_tabs();
    void build_shortcuts();
    void watch_conditions();
    void refresh_once();
    /// Shows and enables the items of a menu and its submenus; true if any item shows.
    bool refresh_menu(BuiltMenu& menu);
    void refresh_context_toolbar();
    void refresh_stages();
    void refresh_tabs(bool force);
    void refresh_shortcuts();
    void trigger_shortcut(const Shortcut& shortcut);

    /// True if a `when` (none: always) is true now.
    [[nodiscard]] bool holds(const std::optional<context::WhenExpression>& when) const;
    /// The command exists on the bus or in the registry.
    [[nodiscard]] bool known(std::string_view command) const;
    [[nodiscard]] QAction* make_command_action(const std::string& command, const QString& title,
                                               bool with_icon, QObject* parent);
    [[nodiscard]] QString text(std::string_view key) const;
    [[nodiscard]] QString menu_title(std::string_view id) const;
    /// The first shortcut of a command, for the text of its items.
    [[nodiscard]] QKeySequence shortcut_of(std::string_view command) const;

    Workbench& workbench_;
    Commands& commands_;
    ContributionHosts hosts_;

    std::vector<std::unique_ptr<BuiltMenu>> menus_;
    std::vector<BuiltMenu*> top_menus_;
    std::vector<QPointer<QAction>> shell_menu_additions_;
    std::vector<QMetaObject::Connection> menu_connections_;
    std::vector<Placement> toolbar_;
    QToolButton* overflow_button_ = nullptr;
    QMenu* overflow_menu_ = nullptr;
    QAction* overflow_action_ = nullptr;
    std::vector<Stage> stages_;
    QActionGroup* stage_group_ = nullptr;
    std::map<std::string, ads::CDockWidget*> activity_panels_;
    std::map<std::string, ads::CDockWidget*> tab_panels_;
    std::map<std::string, Tab> tabs_;
    std::vector<std::unique_ptr<Shortcut>> shortcuts_;

    std::vector<Subscription> watches_;
    Subscription events_;
    Subscription language_;
    bool refreshing_ = false;
    bool refresh_again_ = false;
};

}  // namespace fem::shell
