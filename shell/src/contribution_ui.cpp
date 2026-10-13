#include "fem/shell/contribution_ui.h"

#include "fem/core/context/value.h"
#include "fem/core/log/log.h"
#include "fem/shell/icons.h"
#include "fem/shell/keys.h"
#include "fem/shell/stages.h"
#include "fem/shell/strings.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QActionGroup>
#include <QEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSet>
#include <QToolBar>
#include <QToolButton>
#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace fem::shell {

namespace {

/// Menus of modules with a place of their own on the menu bar, in this order, before the others;
/// "tools" comes after the others. Their titles are the strings "menu.<id>" of the shell.
constexpr std::array<std::string_view, 6> stage_menus{"geometry", "mesh",  "materials",
                                                      "steps",    "solve", "results"};
constexpr std::string_view tools_menu = "tools";

/// The order of a menu of modules on the menu bar.
int menu_rank(std::string_view id) {
    const auto stage = std::find(stage_menus.begin(), stage_menus.end(), id);
    if (stage != stage_menus.end()) {
        return static_cast<int>(stage - stage_menus.begin());
    }
    return id == tools_menu ? 200 : 100;
}

QString qname(std::string_view prefix, std::string_view id) {
    return to_qstring(prefix) + to_qstring(id);
}

/// A muted text in the middle of an empty panel of a module.
QLabel* make_placeholder(const QString& text) {
    auto* label = new QLabel(text);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setBackgroundRole(QPalette::Base);
    label->setAutoFillBackground(true);
    label->setMargin(12);
    return label;
}

/// Adds the shortcuts of the actions of a menu and its submenus to `out`.
void collect_shortcuts(const QMenu* menu, QSet<QKeySequence>& out) {
    for (const QAction* action : menu->actions()) {
        for (const QKeySequence& sequence : action->shortcuts()) {
            out.insert(sequence);
        }
        if (action->menu() != nullptr) {
            collect_shortcuts(action->menu(), out);
        }
    }
}

}  // namespace

ContributionUi::ContributionUi(Workbench& workbench, Commands& commands, ContributionHosts hosts,
                               QObject* parent)
    : QObject(parent), workbench_(workbench), commands_(commands), hosts_(std::move(hosts)) {
    declare_stage_key(workbench_.keys);

    QToolBar* toolbar = hosts_.context_toolbar;
    toolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    toolbar->setIconSize(QSize(32, 32));
    overflow_menu_ = new QMenu(toolbar);
    overflow_menu_->setObjectName(QStringLiteral("contextToolbar.moreMenu"));
    overflow_menu_->setToolTipsVisible(true);
    overflow_button_ = new QToolButton(toolbar);
    overflow_button_->setObjectName(QStringLiteral("contextToolbar.more"));
    overflow_button_->setText(QStringLiteral("⋯"));
    overflow_button_->setPopupMode(QToolButton::InstantPopup);
    overflow_button_->setMenu(overflow_menu_);
    overflow_action_ = toolbar->addWidget(overflow_button_);

    stage_group_ = new QActionGroup(this);
    stage_group_->setExclusive(true);

    events_ = workbench_.extensions.on_event([this](const extension::ExtensionEvent& event) {
        using Kind = extension::ExtensionEvent::Kind;
        if (event.kind == Kind::enabled || event.kind == Kind::deactivated) {
            rebuild();
        }
    });
    language_ =
        workbench_.localization.on_language_changed([this](const std::string&) { rebuild(); });
    rebuild();
}

ContributionUi::~ContributionUi() {
    // The listeners go first: what they would refresh is being destroyed.
    watches_.clear();
    events_.reset();
    language_.reset();
}

void ContributionUi::rebuild() {
    watches_.clear();
    clear();
    overflow_button_->setToolTip(text("contextToolbar.more"));
    build_shortcuts();
    build_menus();
    build_context_toolbar();
    build_stages();
    build_activities();
    build_tabs();
    watch_conditions();
    refresh();
    Q_EMIT rebuilt();
}

void ContributionUi::clear() {
    for (const QMetaObject::Connection& connection : menu_connections_) {
        disconnect(connection);
    }
    menu_connections_.clear();
    for (const QPointer<QAction>& action : shell_menu_additions_) {
        delete action.data();
    }
    shell_menu_additions_.clear();
    // Deleting a menu deletes its submenus: the pointers of those turn null.
    for (const std::unique_ptr<BuiltMenu>& menu : menus_) {
        if (!menu->shell) {
            delete menu->menu.data();
        }
    }
    menus_.clear();
    top_menus_.clear();

    for (const Placement& placement : toolbar_) {
        delete placement.action.data();
        delete placement.overflow.data();
    }
    toolbar_.clear();

    for (const Stage& stage : stages_) {
        delete stage.action.data();
    }
    stages_.clear();

    for (const std::unique_ptr<Shortcut>& shortcut : shortcuts_) {
        delete shortcut->action.data();
    }
    shortcuts_.clear();
    // Panels of modules stay until `build_activities` and `build_tabs` know which are gone.
}

// --- Shortcuts -----------------------------------------------------------------------------------

void ContributionUi::build_shortcuts() {
    const auto log = log::logger("shell");
    // Shortcuts of the shell win: two actions with one shortcut would both be ambiguous.
    QSet<QKeySequence> reserved;
    for (const auto& [id, menu] : hosts_.shell_menus) {
        collect_shortcuts(menu, reserved);
    }
    std::map<QString, Shortcut*> by_key;
    for (const extension::KeybindingContribution* binding :
         workbench_.extensions.registry().keybindings()) {
        const QKeySequence sequence = key_sequence(binding->key);
        if (sequence.isEmpty()) {
            log->warn("module '{}': '{}' is not a shortcut", binding->owner, binding->key);
            continue;
        }
        if (reserved.contains(sequence)) {
            log->warn("module '{}': the shortcut {} of '{}' belongs to the shell", binding->owner,
                      binding->key, binding->command);
            continue;
        }
        const QString portable = sequence.toString(QKeySequence::PortableText);
        Shortcut*& shortcut = by_key[portable];
        if (shortcut == nullptr) {
            auto made = std::make_unique<Shortcut>();
            made->sequence = sequence;
            auto* action = new QAction(hosts_.window);
            action->setObjectName(QStringLiteral("key.") + portable);
            action->setShortcut(sequence);
            action->setShortcutContext(Qt::WindowShortcut);
            hosts_.window->addAction(action);
            Shortcut* raw = made.get();
            connect(action, &QAction::triggered, this, [this, raw] { trigger_shortcut(*raw); });
            made->action = action;
            shortcut = raw;
            shortcuts_.push_back(std::move(made));
        }
        shortcut->bindings.push_back({.command = binding->command, .when = binding->when});
    }
}

void ContributionUi::trigger_shortcut(const Shortcut& shortcut) {
    // The first binding whose condition holds: one key may do different things in two contexts.
    for (const Binding& binding : shortcut.bindings) {
        if (holds(binding.when) && known(binding.command)) {
            (void)commands_.run(binding.command);
            return;
        }
    }
}

void ContributionUi::refresh_shortcuts() {
    for (const std::unique_ptr<Shortcut>& shortcut : shortcuts_) {
        const bool on = std::any_of(shortcut->bindings.begin(), shortcut->bindings.end(),
                                    [this](const Binding& binding) {
                                        return holds(binding.when) && known(binding.command);
                                    });
        shortcut->action->setEnabled(on);
    }
}

QKeySequence ContributionUi::shortcut_of(std::string_view command) const {
    for (const std::unique_ptr<Shortcut>& shortcut : shortcuts_) {
        for (const Binding& binding : shortcut->bindings) {
            if (binding.command == command) {
                return shortcut->sequence;
            }
        }
    }
    return {};
}

// --- Menus ---------------------------------------------------------------------------------------

void ContributionUi::build_menus() {
    const extension::ContributionRegistry& registry = workbench_.extensions.registry();
    std::map<std::string, BuiltMenu*, std::less<>> by_path;
    // The node of a location, with the nodes of its parents.
    const auto node = [&](const std::string& path, const auto& self) -> BuiltMenu& {
        if (const auto found = by_path.find(path); found != by_path.end()) {
            return *found->second;
        }
        auto made = std::make_unique<BuiltMenu>();
        made->path = path;
        BuiltMenu* raw = made.get();
        menus_.push_back(std::move(made));
        by_path.emplace(path, raw);
        const std::size_t slash = path.rfind('/');
        if (slash == std::string::npos) {
            raw->shell = hosts_.shell_menus.contains(path);
            top_menus_.push_back(raw);
        } else {
            self(path.substr(0, slash), self).children.push_back(raw);
        }
        return *raw;
    };
    for (const extension::MenuContribution* entry : registry.menus()) {
        if (!known(entry->command)) {
            continue;
        }
        node(entry->location, node)
            .items.push_back({.command = entry->command,
                              .when = entry->when,
                              .group = parse_menu_group(entry->group),
                              .title = commands_.title(entry->command)});
    }

    const auto fill = [&](BuiltMenu& built, const auto& self) -> void {
        QMenu* menu = built.menu;
        // A reason may change while the value of its condition does not: fresh when shown.
        menu_connections_.push_back(
            connect(menu, &QMenu::aboutToShow, this, [this] { refresh(); }));
        std::sort(built.items.begin(), built.items.end(),
                  [](const Placement& a, const Placement& b) {
                      return menu_item_before(a.group, a.title, b.group, b.title);
                  });
        // In a menu of the shell, what modules add is deleted on the next rebuild.
        const auto add = [&](QAction* action) {
            menu->addAction(action);
            if (built.shell) {
                shell_menu_additions_.emplace_back(action);
            }
        };
        const auto add_separator = [&] {
            auto* separator = new QAction(menu);
            separator->setSeparator(true);
            add(separator);
        };
        if (built.shell && (!built.items.empty() || !built.children.empty())) {
            add_separator();
        }
        const std::string* group = nullptr;
        for (Placement& item : built.items) {
            if (group != nullptr && *group != item.group.name) {
                add_separator();
            }
            group = &item.group.name;
            item.action = make_command_action(item.command, item.title, true, menu);
            add(item.action);
        }
        const auto last_part = [](const std::string& path) {
            return std::string_view(path).substr(path.rfind('/') + 1);
        };
        std::sort(built.children.begin(), built.children.end(),
                  [&](const BuiltMenu* a, const BuiltMenu* b) {
                      return QString::localeAwareCompare(menu_title(last_part(a->path)),
                                                         menu_title(last_part(b->path))) < 0;
                  });
        if (!built.items.empty() && !built.children.empty()) {
            add_separator();
        }
        for (BuiltMenu* child : built.children) {
            child->menu = new QMenu(menu_title(last_part(child->path)), menu);
            child->menu->setObjectName(qname("menu.", child->path));
            child->menu->setToolTipsVisible(true);
            // Not an addition to delete: deleting the submenu takes its action away.
            menu->addAction(child->menu->menuAction());
            self(*child, self);
        }
    };

    std::stable_sort(
        top_menus_.begin(), top_menus_.end(), [this](const BuiltMenu* a, const BuiltMenu* b) {
            const int rank_a = menu_rank(a->path);
            const int rank_b = menu_rank(b->path);
            if (rank_a != rank_b) {
                return rank_a < rank_b;
            }
            return QString::localeAwareCompare(menu_title(a->path), menu_title(b->path)) < 0;
        });
    QAction* help = nullptr;
    if (const auto found = hosts_.shell_menus.find("help"); found != hosts_.shell_menus.end()) {
        help = found->second->menuAction();
    }
    for (BuiltMenu* top : top_menus_) {
        if (top->shell) {
            top->menu = hosts_.shell_menus.find(top->path)->second;
        } else {
            top->menu = new QMenu(menu_title(top->path), hosts_.menu_bar);
            top->menu->setObjectName(qname("menu.", top->path));
            top->menu->setToolTipsVisible(true);
            hosts_.menu_bar->insertMenu(help, top->menu);
        }
        fill(*top, fill);
    }
}

bool ContributionUi::refresh_menu(BuiltMenu& built) {
    bool any = false;
    for (Placement& item : built.items) {
        const bool shown = holds(item.when);
        item.action->setVisible(shown);
        const Availability availability = commands_.availability(item.command);
        item.action->setEnabled(availability.available);
        item.action->setToolTip(availability.available ? item.title : availability.reason);
        any = any || shown;
    }
    for (BuiltMenu* child : built.children) {
        any = refresh_menu(*child) || any;
    }
    if (!built.shell && built.menu != nullptr) {
        built.menu->menuAction()->setVisible(any);
    }
    return any;
}

QString ContributionUi::menu_title(std::string_view id) const {
    if (const auto found = workbench_.localization.find(strings_owner, "menu." + std::string(id))) {
        return to_qstring(*found);
    }
    for (const extension::SubmenuContribution* submenu :
         workbench_.extensions.registry().submenus()) {
        if (submenu->id == id) {
            return to_qstring(
                workbench_.localization.manifest_text(submenu->owner, submenu->title));
        }
    }
    return to_qstring(id);
}

// --- Context toolbar -----------------------------------------------------------------------------

void ContributionUi::build_context_toolbar() {
    for (const extension::ContextToolbarContribution* entry :
         workbench_.extensions.registry().context_toolbar()) {
        if (!known(entry->command)) {
            continue;
        }
        toolbar_.push_back({.command = entry->command,
                            .when = entry->when,
                            .group = parse_menu_group(entry->group),
                            .title = commands_.title(entry->command)});
    }
    // Rare commands (the group "more") after all others: they always go to "⋯".
    std::stable_sort(toolbar_.begin(), toolbar_.end(), [](const Placement& a, const Placement& b) {
        const bool rare_a = a.group.name == overflow_group;
        const bool rare_b = b.group.name == overflow_group;
        if (rare_a != rare_b) {
            return rare_b;
        }
        return menu_item_before(a.group, a.title, b.group, b.title);
    });
    for (Placement& placement : toolbar_) {
        placement.action =
            make_command_action(placement.command, placement.title, true, hosts_.context_toolbar);
        if (placement.action->icon().isNull()) {
            placement.action->setIcon(default_module_icon());
        }
        hosts_.context_toolbar->insertAction(overflow_action_, placement.action);
        // The reason in its tooltip is made fresh when the tooltip shows.
        if (QWidget* button = hosts_.context_toolbar->widgetForAction(placement.action)) {
            button->installEventFilter(this);
        }
        placement.overflow =
            make_command_action(placement.command, placement.title, true, overflow_menu_);
        overflow_menu_->addAction(placement.overflow);
    }
}

void ContributionUi::refresh_context_toolbar() {
    int on_bar = 0;
    bool overflow = false;
    for (Placement& placement : toolbar_) {
        const bool shown = holds(placement.when);
        const bool rare = placement.group.name == overflow_group;
        const bool bar = shown && !rare && on_bar < max_toolbar_commands;
        on_bar += bar ? 1 : 0;
        overflow = overflow || (shown && !bar);
        placement.action->setVisible(bar);
        placement.overflow->setVisible(shown && !bar);
        const Availability availability = commands_.availability(placement.command);
        for (QAction* action : {placement.action.data(), placement.overflow.data()}) {
            action->setEnabled(availability.available);
            action->setToolTip(availability.available ? placement.title : availability.reason);
        }
    }
    overflow_action_->setVisible(overflow);
    hosts_.context_toolbar->setVisible(on_bar > 0 || overflow);
}

// --- Stages --------------------------------------------------------------------------------------

void ContributionUi::build_stages() {
    const extension::ContributionRegistry& registry = workbench_.extensions.registry();
    std::vector<const extension::StageContribution*> rejected;
    const std::vector<StageEntry> order = stage_order(registry, &rejected);
    for (const extension::StageContribution* stage : rejected) {
        log::logger("shell")->warn(
            "module '{}': stage '{}' is not shown, '{}' is not a standard stage", stage->owner,
            stage->id, stage->anchor);
    }
    for (const StageEntry& entry : order) {
        auto* action =
            new QAction(stage_title(workbench_.localization, registry, entry.id), hosts_.stage_bar);
        action->setObjectName(qname("stage.", entry.id));
        action->setCheckable(true);
        action->setActionGroup(stage_group_);
        if (entry.contribution != nullptr) {
            const QIcon icon = module_icon(workbench_.extensions, entry.contribution->owner,
                                           entry.contribution->icon, false);
            if (!icon.isNull()) {
                action->setIcon(icon);
            }
        }
        connect(action, &QAction::triggered, this, [this, id = entry.id] {
            workbench_.keys.set(stage_key, context::ContextValue(id));
        });
        hosts_.stage_bar->addAction(action);
        stages_.push_back(
            {.id = entry.id,
             .when = entry.contribution != nullptr ? entry.contribution->when : std::nullopt,
             .action = action});
    }
}

void ContributionUi::refresh_stages() {
    const context::ContextValue& current = workbench_.keys.value(stage_key);
    bool current_shown = false;
    for (Stage& stage : stages_) {
        const bool shown = holds(stage.when);
        stage.action->setVisible(shown);
        const bool is_current = current == context::ContextValue(stage.id);
        stage.action->setChecked(is_current && shown);
        current_shown = current_shown || (is_current && shown);
    }
    if (!current_shown) {
        // The stage is gone (its module was disabled, its condition turned false): back to the
        // first one. Setting the key refreshes again.
        workbench_.keys.set(stage_key, context::ContextValue(standard_stages.front()));
    }
}

// --- Panels --------------------------------------------------------------------------------------

void ContributionUi::build_activities() {
    const extension::ContributionRegistry& registry = workbench_.extensions.registry();
    std::map<std::string, const extension::ActivityBarContribution*> wanted;
    for (const extension::ActivityBarContribution* activity : registry.activity_bar()) {
        wanted.emplace(activity->id, activity);
    }
    for (auto it = activity_panels_.begin(); it != activity_panels_.end();) {
        if (wanted.contains(it->first)) {
            ++it;
            continue;
        }
        hosts_.docks->removeDockWidget(it->second);
        delete it->second;
        it = activity_panels_.erase(it);
    }
    hosts_.activity_bar->clear();
    for (const extension::ActivityBarContribution* activity : registry.activity_bar()) {
        const QString title =
            to_qstring(workbench_.localization.manifest_text(activity->owner, activity->title));
        ads::CDockWidget*& panel = activity_panels_[activity->id];
        if (panel == nullptr) {
            panel = new ads::CDockWidget(hosts_.docks, qname("activity:", activity->id));
            panel->setObjectName(qname("activity:", activity->id));
            panel->setWidget(make_placeholder(text("zone.empty")),
                             ads::CDockWidget::ForceNoScrollArea);
            ads::CDockAreaWidget* area =
                hosts_.side_panel != nullptr ? hosts_.side_panel->dockAreaWidget() : nullptr;
            if (area != nullptr) {
                hosts_.docks->addDockWidgetTabToArea(panel, area);
            } else {
                hosts_.docks->addDockWidget(ads::LeftDockWidgetArea, panel);
            }
            // Closed until its icon is clicked.
            panel->toggleView(false);
        }
        panel->setWindowTitle(title);
        QAction* toggle = panel->toggleViewAction();
        toggle->setObjectName(qname("activity.", activity->id));
        toggle->setText(title);
        toggle->setToolTip(title);
        toggle->setIcon(module_icon(workbench_.extensions, activity->owner, activity->icon, true));
        hosts_.activity_bar->addAction(toggle);
    }
}

void ContributionUi::build_tabs() {
    const extension::ContributionRegistry& registry = workbench_.extensions.registry();
    std::map<std::string, const extension::BottomPanelTabContribution*> wanted;
    for (const extension::BottomPanelTabContribution* tab : registry.bottom_panel_tabs()) {
        wanted.emplace(tab->id, tab);
    }
    for (auto it = tab_panels_.begin(); it != tab_panels_.end();) {
        if (wanted.contains(it->first)) {
            ++it;
            continue;
        }
        hosts_.docks->removeDockWidget(it->second);
        delete it->second;
        tabs_.erase(it->first);
        it = tab_panels_.erase(it);
    }
    for (const extension::BottomPanelTabContribution* tab : registry.bottom_panel_tabs()) {
        ads::CDockWidget*& panel = tab_panels_[tab->id];
        if (panel == nullptr) {
            panel = new ads::CDockWidget(hosts_.docks, qname("tab:", tab->id));
            panel->setObjectName(qname("tab:", tab->id));
            panel->setWidget(make_placeholder(text("zone.empty")),
                             ads::CDockWidget::ForceNoScrollArea);
            ads::CDockAreaWidget* area =
                hosts_.bottom_panel != nullptr ? hosts_.bottom_panel->dockAreaWidget() : nullptr;
            if (area != nullptr) {
                // A new tab joins the others without taking the front from the one there.
                ads::CDockWidget* front = area->currentDockWidget();
                hosts_.docks->addDockWidgetTabToArea(panel, area);
                if (front != nullptr) {
                    area->setCurrentDockWidget(front);
                }
            } else {
                hosts_.docks->addDockWidget(ads::BottomDockWidgetArea, panel);
            }
        }
        panel->setWindowTitle(
            to_qstring(workbench_.localization.manifest_text(tab->owner, tab->title)));
        tabs_[tab->id].when = tab->when;
    }
}

void ContributionUi::refresh_tabs(bool force) {
    for (auto& [id, tab] : tabs_) {
        const bool shown = holds(tab.when);
        if (!force && tab.shown == shown) {
            continue;  // Opened or closed by hand since: left as it is.
        }
        tab.shown = shown;
        if (ads::CDockWidget* panel = tab_panels_[id]; panel->isClosed() == shown) {
            panel->toggleView(shown);
        }
    }
}

void ContributionUi::apply_tab_conditions() {
    refresh_tabs(true);
}

bool ContributionUi::overflow_shown() const {
    return overflow_action_->isVisible();
}

std::vector<ads::CDockWidget*> ContributionUi::module_panels() const {
    std::vector<ads::CDockWidget*> panels;
    for (const auto& [id, panel] : activity_panels_) {
        panels.push_back(panel);
    }
    for (const auto& [id, panel] : tab_panels_) {
        panels.push_back(panel);
    }
    return panels;
}

// --- Context menus -------------------------------------------------------------------------------

QMenu* ContributionUi::context_menu(std::string_view location, QWidget* parent) const {
    struct Item {
        std::string command;
        MenuGroup group;
        QString title;
    };
    std::vector<Item> items;
    for (const extension::MenuContribution* entry :
         workbench_.extensions.registry().context_menus()) {
        if (entry->location == location && known(entry->command) && holds(entry->when)) {
            items.push_back({.command = entry->command,
                             .group = parse_menu_group(entry->group),
                             .title = commands_.title(entry->command)});
        }
    }
    if (items.empty()) {
        return nullptr;
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return menu_item_before(a.group, a.title, b.group, b.title);
    });
    auto* menu = new QMenu(parent);
    menu->setObjectName(qname("contextMenu.", location));
    menu->setToolTipsVisible(true);
    const std::string* group = nullptr;
    for (const Item& item : items) {
        if (group != nullptr && *group != item.group.name) {
            menu->addSeparator();
        }
        group = &item.group.name;
        QAction* action = menu->addAction(item.title);
        action->setObjectName(to_qstring(item.command));
        const Availability availability = commands_.availability(item.command);
        action->setEnabled(availability.available);
        action->setToolTip(availability.available ? item.title : availability.reason);
        Commands* commands = &commands_;
        connect(action, &QAction::triggered, menu,
                [commands, id = item.command] { (void)commands->run(id); });
    }
    return menu;
}

bool ContributionUi::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        refresh();
    }
    return QObject::eventFilter(watched, event);
}

// --- Conditions ----------------------------------------------------------------------------------

void ContributionUi::watch_conditions() {
    std::map<std::string, context::WhenExpression> conditions;
    const auto add = [&conditions](const std::optional<context::WhenExpression>& when) {
        if (when) {
            conditions.emplace(when->to_string(), *when);
        }
    };
    for (const std::unique_ptr<BuiltMenu>& menu : menus_) {
        for (const Placement& item : menu->items) {
            add(item.when);
        }
    }
    for (const Placement& placement : toolbar_) {
        add(placement.when);
    }
    for (const Stage& stage : stages_) {
        add(stage.when);
        // The check mark follows the current stage.
        add(context::WhenExpression::parse(std::string(stage_key) + " == '" + stage.id + "'"));
    }
    for (const auto& [id, tab] : tabs_) {
        add(tab.when);
    }
    for (const std::unique_ptr<Shortcut>& shortcut : shortcuts_) {
        for (const Binding& binding : shortcut->bindings) {
            add(binding.when);
        }
    }
    for (const extension::CommandContribution* command :
         workbench_.extensions.registry().commands()) {
        add(command->enablement);
    }
    for (auto& [text, expression] : conditions) {
        watches_.push_back(
            workbench_.keys.watch(std::move(expression), [this](bool) { refresh(); }));
    }
}

void ContributionUi::refresh() {
    // Refreshing may set the stage, which asks for another refresh: run it after this one.
    if (refreshing_) {
        refresh_again_ = true;
        return;
    }
    refreshing_ = true;
    do {
        refresh_again_ = false;
        refresh_once();
    } while (refresh_again_);
    refreshing_ = false;
}

void ContributionUi::refresh_once() {
    for (BuiltMenu* top : top_menus_) {
        (void)refresh_menu(*top);
    }
    refresh_context_toolbar();
    refresh_shortcuts();
    refresh_tabs(false);
    refresh_stages();
}

bool ContributionUi::holds(const std::optional<context::WhenExpression>& when) const {
    return !when || workbench_.keys.evaluate(*when);
}

bool ContributionUi::known(std::string_view command) const {
    return workbench_.bus.find_command(command) != nullptr ||
           workbench_.extensions.registry().find_command(command) != nullptr;
}

QAction* ContributionUi::make_command_action(const std::string& command, const QString& title,
                                             bool with_icon, QObject* parent) {
    auto* action = new QAction(title, parent);
    action->setObjectName(to_qstring(command));
    if (with_icon) {
        if (const extension::CommandContribution* contribution =
                workbench_.extensions.registry().find_command(command)) {
            action->setIcon(
                module_icon(workbench_.extensions, contribution->owner, contribution->icon, false));
        }
    }
    if (const QKeySequence sequence = shortcut_of(command); !sequence.isEmpty()) {
        // For the text of the item only: the shortcut itself is an action of the window that
        // looks at the `when` of the binding.
        action->setShortcut(sequence);
        action->setShortcutContext(Qt::WidgetShortcut);
    }
    connect(action, &QAction::triggered, this, [this, command] { (void)commands_.run(command); });
    return action;
}

QString ContributionUi::text(std::string_view key) const {
    return shell::text(workbench_.localization, key);
}

}  // namespace fem::shell
