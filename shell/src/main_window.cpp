#include "fem/shell/main_window.h"

#include "fem/core/context/when.h"
#include "fem/core/log/log.h"
#include "fem/core/product.h"
#include "fem/shell/about_dialog.h"
#include "fem/shell/brand.h"
#include "fem/shell/command_palette.h"
#include "fem/shell/commands.h"
#include "fem/shell/contribution_ui.h"
#include "fem/shell/extensions_panel.h"
#include "fem/shell/icons.h"
#include "fem/shell/journal_panel.h"
#include "fem/shell/keys.h"
#include "fem/shell/model_tree.h"
#include "fem/shell/problems.h"
#include "fem/shell/project_actions.h"
#include "fem/shell/property_panel.h"
#include "fem/shell/python_console.h"
#include "fem/shell/required_modules_bar.h"
#include "fem/shell/settings_panel.h"
#include "fem/shell/strings.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QRegularExpression>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>

namespace fem::shell {

namespace {

/// The version of the layouts the window writes; a layout of another version is not restored.
constexpr int layout_version = 1;

/// The size the window opens with the first time.
constexpr QSize default_size(1280, 800);

QString qstring(std::string_view id) {
    return QString::fromLatin1(id.data(), static_cast<qsizetype>(id.size()));
}

/// A muted text in the middle of an empty zone.
QLabel* make_placeholder(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setBackgroundRole(QPalette::Base);
    label->setAutoFillBackground(true);
    label->setMargin(12);
    return label;
}

}  // namespace

MainWindow::MainWindow(Workbench workbench, QWidget* parent)
    : QMainWindow(parent),
      workbench_(workbench),
      localization_(workbench.localization),
      workspaces_(workbench.workspaces) {
    setObjectName(QStringLiteral("mainWindow"));
    setWindowIcon(brand::application_icon());
    resize(default_size);

    commands_ = std::make_unique<Commands>(workbench_);
    commands_->set_report([this](const QString& message) { set_prompt(message); });
    build_zones();
    build_menus();
    build_contributions();
    retranslate();
    apply_default_sizes();
    default_layout_ = layout();

    if (workspaces_.session() && restore_layout(*workspaces_.session())) {
        (void)restoreGeometry(workspaces_.geometry());
    }
    language_ = localization_.on_language_changed([this](const std::string&) { retranslate(); });
}

MainWindow::~MainWindow() {
    // The tree refreshes what modules contribute when its selection changes: not any more.
    tree_->set_on_selection({});
    tree_->set_module_menu({});
    // What modules contribute goes before the widgets it is in.
    history_watches_.clear();
    model_changed_.reset();
    extension_events_.reset();
    contributions_.reset();
}

void MainWindow::build_zones() {
    // Before the dock manager exists: the flags are read when it is created.
    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    // Layouts are kept as readable XML in the JSON of workspaces.
    ads::CDockManager::setConfigFlag(ads::CDockManager::XmlCompressionEnabled, false);

    const auto add_bar = [this](std::string_view id, std::string_view title_key,
                                Qt::ToolBarArea area, QSize minimum) {
        auto* bar = new QToolBar(this);
        bar->setObjectName(qstring(id));
        // Empty until modules add stages and activities (task 1.12): keep the zone visible.
        bar->setMinimumSize(minimum);
        bar->setFloatable(false);
        addToolBar(area, bar);
        zones_.push_back({.id = id, .title_key = title_key, .widget = bar, .bar = bar});
        return bar;
    };
    add_bar(zone::stage_bar, "zone.stageBar", Qt::TopToolBarArea, QSize(36, 36));
    QToolBar* activities =
        add_bar(zone::activity_bar, "zone.activityBar", Qt::LeftToolBarArea, QSize(44, 44));
    activities->setIconSize(QSize(24, 24));

    docks_ = new ads::CDockManager(this);
    docks_->setObjectName(QStringLiteral("dockManager"));

    const auto add_dock = [this](std::string_view id, std::string_view title_key,
                                 QWidget* content) {
        auto* dock = new ads::CDockWidget(docks_, qstring(id));
        dock->setObjectName(qstring(id));
        dock->setWidget(content, ads::CDockWidget::ForceNoScrollArea);
        zones_.push_back({.id = id, .title_key = title_key, .widget = dock, .dock = dock});
        return dock;
    };
    const auto add_empty_dock = [&](std::string_view id, std::string_view title_key) {
        QLabel* placeholder = make_placeholder(nullptr);
        ads::CDockWidget* dock = add_dock(id, title_key, placeholder);
        zones_.back().placeholder = placeholder;
        return dock;
    };

    // The 3D view: the central area, always there. A stub until the viewport module (task 3.7),
    // under the context toolbar with the commands of the current context.
    auto* viewport_content = new QWidget;
    auto* viewport_layout = new QVBoxLayout(viewport_content);
    viewport_layout->setContentsMargins(0, 0, 0, 0);
    viewport_layout->setSpacing(0);
    // The bar "The model needs the modules…", hidden until a model needs them.
    required_ = new RequiredModulesBar(workbench_, viewport_content);
    required_->set_report([this](const QString& message) { set_prompt(message); });
    viewport_layout->addWidget(required_);
    context_toolbar_ = new QToolBar(viewport_content);
    context_toolbar_->setObjectName(QStringLiteral("contextToolbar"));
    viewport_layout->addWidget(context_toolbar_);
    QLabel* viewport_stub = make_placeholder(viewport_content);
    QFont font = viewport_stub->font();
    font.setPointSizeF(font.pointSizeF() * 1.6);
    viewport_stub->setFont(font);
    viewport_layout->addWidget(viewport_stub, 1);
    ads::CDockWidget* viewport = add_dock(zone::viewport, "zone.viewport", viewport_content);
    zones_.back().placeholder = viewport_stub;
    viewport->setFeature(ads::CDockWidget::NoTab, true);
    ads::CDockAreaWidget* central = docks_->setCentralWidget(viewport);

    // The model tree and the properties of what is selected in it; the settings as a tab next to
    // the properties, closed until opened.
    tree_ = new ModelTreePanel(workbench_, *commands_);
    ads::CDockWidget* tree = add_dock(zone::model_tree, "zone.modelTree", tree_);
    ads::CDockAreaWidget* left = docks_->addDockWidget(ads::LeftDockWidgetArea, tree, central);
    // The module manager: a tab next to the tree, closed until its activity is clicked.
    extensions_ = new ExtensionsPanel(workbench_);
    ads::CDockWidget* extensions = add_dock(zone::extensions, "zone.extensions", extensions_);
    docks_->addDockWidgetTabToArea(extensions, left);
    extensions->toggleView(false);
    left->setCurrentDockWidget(tree);
    properties_ = new PropertyPanel(workbench_);
    ads::CDockWidget* properties = add_dock(zone::properties, "zone.properties", properties_);
    ads::CDockAreaWidget* side =
        docks_->addDockWidget(ads::RightDockWidgetArea, properties, central);
    settings_ = new SettingsPanel(workbench_);
    ads::CDockWidget* settings = add_dock(zone::settings, "zone.settings", settings_);
    docks_->addDockWidgetTabToArea(settings, side);
    settings->toggleView(false);
    side->setCurrentDockWidget(properties);
    connect(tree_, &ModelTreePanel::selection_changed, this,
            [this] { properties_->show_object(tree_->selected()); });

    // The prompt line, under the tree, the view and the properties, over the bottom panel.
    auto* prompt_line = new QWidget;
    auto* prompt_layout = new QHBoxLayout(prompt_line);
    prompt_layout->setContentsMargins(10, 3, 10, 3);
    prompt_ = new QLabel(prompt_line);
    prompt_->setObjectName(QStringLiteral("promptText"));
    prompt_->setTextFormat(Qt::PlainText);
    prompt_layout->addWidget(prompt_, 1);
    ads::CDockWidget* prompt = add_dock(zone::prompt_line, "zone.prompt", prompt_line);
    prompt->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromContent);
    ads::CDockAreaWidget* prompt_area = docks_->addDockWidget(ads::BottomDockWidgetArea, prompt);
    prompt_area->setDockAreaFlag(ads::CDockAreaWidget::HideSingleWidgetTitleBar, true);

    // The bottom panel: a tab per dock, the journal in front.
    journal_ = new JournalPanel(workbench_);
    journal_->set_report([this](const QString& message) { set_prompt(message); });
    ads::CDockWidget* journal = add_dock(zone::journal, "zone.journal", journal_);
    ads::CDockAreaWidget* bottom = docks_->addDockWidget(ads::BottomDockWidgetArea, journal);
    console_ = new PythonConsolePanel(workbench_);
    docks_->addDockWidgetTabToArea(add_dock(zone::python_console, "zone.console", console_),
                                   bottom);
    docks_->addDockWidgetTabToArea(add_empty_dock(zone::jobs, "zone.jobs"), bottom);
    problems_ = new ProblemsPanel;
    docks_->addDockWidgetTabToArea(add_dock(zone::problems, "zone.problems", problems_), bottom);
    connect(problems_, &ProblemsPanel::changed, this, &MainWindow::retranslate);
    bottom->setCurrentDockWidget(journal);

    // The style sheet of ADS draws splitters, side bar lines and the labels of inactive tabs in
    // palette(dark): a heavy line on the light theme and unreadable text on the dark one. Its rules
    // are changed in place (rules appended after them do not win), once the panels exist (a sheet
    // changed before that does not reach them): inactive tabs in the muted text colour, lines in
    // mid.
    QString sheet = docks_->styleSheet();
    sheet.replace(QRegularExpression(
                      QStringLiteral(R"((CDockWidgetTab QLabel\s*\{\s*color:\s*)palette\(dark\))")),
                  QStringLiteral("\\1palette(placeholder-text)"));
    sheet.replace(QStringLiteral("palette(dark)"), QStringLiteral("palette(mid)"));
    docks_->setStyleSheet(sheet);
}

void MainWindow::apply_default_sizes() {
    const auto area = [this](std::string_view id) {
        const Zone* zone = find_zone(id);
        return zone && zone->dock ? zone->dock->dockAreaWidget() : nullptr;
    };
    // Tree | 3D view | properties, and the main row | prompt line | bottom panel.
    if (ads::CDockAreaWidget* tree = area(zone::model_tree)) {
        docks_->setSplitterSizes(tree, {260, default_size.width() - 260 - 300, 300});
    }
    if (ads::CDockAreaWidget* prompt = area(zone::prompt_line)) {
        const int line = prompt->minimumSizeHint().height();
        docks_->setSplitterSizes(prompt, {default_size.height() - line - 220, line, 220});
    }
}

void MainWindow::build_menus() {
    file_menu_ = menuBar()->addMenu(QString());
    file_menu_->setObjectName(QStringLiteral("menu.file"));
    project_ = new ProjectActions(workbench_, this);
    connect(project_, &ProjectActions::problem, this, [this](const QString& message) {
        problems_->add(message);
        set_prompt(message);
    });
    connect(project_, &ProjectActions::message, this,
            [this](const QString& message) { set_prompt(message); });
    connect(project_, &ProjectActions::opened, this, &MainWindow::check_required_modules);
    file_menu_->addAction(project_->open_action());
    file_menu_->addSeparator();
    file_menu_->addAction(project_->save_action());
    file_menu_->addAction(project_->save_as_action());
    file_menu_->addAction(project_->clean_action());
    file_menu_->addSeparator();
    file_menu_->addAction(journal_->save_action());
    file_menu_->addSeparator();
    settings_action_ = file_menu_->addAction(QString(), this, &MainWindow::show_settings);
    settings_action_->setObjectName(QStringLiteral("app.settings"));
    // On macOS the item goes to the application menu, as Cmd+,.
    settings_action_->setMenuRole(QAction::PreferencesRole);
    settings_action_->setShortcut(key_sequence("Mod+,"));
    file_menu_->addSeparator();
    quit_ = file_menu_->addAction(QString(), this, &QWidget::close);
    quit_->setObjectName(QStringLiteral("app.quit"));
    // On macOS the item goes to the application menu, as Cmd+Q.
    quit_->setMenuRole(QAction::QuitRole);
    quit_->setShortcut(key_sequence("Mod+Q"));

    // Undo and redo of the Command Bus; available by the keys of the core.
    edit_menu_ = menuBar()->addMenu(QString());
    edit_menu_->setObjectName(QStringLiteral("menu.edit"));
    const auto history_step = [this](bool undo) {
        try {
            (void)(undo ? workbench_.bus.undo() : workbench_.bus.redo());
        } catch (const std::exception& error) {
            log::logger("shell")->warn("{} failed: {}", undo ? "undo" : "redo", error.what());
            set_prompt(to_qstring(error.what()));
        }
        update_history();
    };
    undo_ = edit_menu_->addAction(QString(), this, [history_step] { history_step(true); });
    undo_->setObjectName(QStringLiteral("app.undo"));
    undo_->setShortcuts(QKeySequence::Undo);
    redo_ = edit_menu_->addAction(QString(), this, [history_step] { history_step(false); });
    redo_->setObjectName(QStringLiteral("app.redo"));
    redo_->setShortcuts(QKeySequence::Redo);

    view_menu_ = menuBar()->addMenu(QString());
    view_menu_->setObjectName(QStringLiteral("menu.view"));
    palette_action_ = view_menu_->addAction(QString(), this, &MainWindow::show_palette);
    palette_action_->setObjectName(QStringLiteral("view.commandPalette"));
    palette_action_->setShortcut(key_sequence("Mod+Shift+P"));
    view_menu_->addSeparator();
    for (const Zone& zone : zones_) {
        QAction* toggle = zone.dock  ? zone.dock->toggleViewAction()
                          : zone.bar ? zone.bar->toggleViewAction()
                                     : nullptr;
        if (toggle == nullptr || zone.id == zone::viewport) {
            continue;  // The 3D view is the central area: it is always there.
        }
        toggle->setObjectName(QStringLiteral("view.") + qstring(zone.id));
        view_menu_->addAction(toggle);
    }
    view_menu_->addSeparator();
    workspace_menu_ = view_menu_->addMenu(QString());
    workspace_menu_->setObjectName(QStringLiteral("menu.workspaces"));
    connect(workspace_menu_, &QMenu::aboutToShow, this, &MainWindow::rebuild_workspace_menu);
    view_menu_->addSeparator();
    full_screen_ = view_menu_->addAction(QString());
    full_screen_->setObjectName(QStringLiteral("view.fullScreen"));
    full_screen_->setCheckable(true);
    full_screen_->setShortcut(QKeySequence::FullScreen);
    connect(full_screen_, &QAction::toggled, this,
            [this](bool on) { setWindowState(windowState().setFlag(Qt::WindowFullScreen, on)); });

    help_menu_ = menuBar()->addMenu(QString());
    help_menu_->setObjectName(QStringLiteral("menu.help"));
    about_ = help_menu_->addAction(QString(), this, [this] { (void)show_about(); });
    about_->setObjectName(QStringLiteral("app.about"));
    about_->setMenuRole(QAction::AboutRole);
}

void MainWindow::build_contributions() {
    const auto dock = [this](std::string_view id) { return find_zone(id)->dock; };
    // The activities of the shell open and close their panels.
    QList<QAction*> shell_activities;
    for (const auto& [id, icon] :
         {std::pair{zone::model_tree, "model"}, std::pair{zone::extensions, "extensions"},
          std::pair{zone::settings, "settings"}}) {
        QAction* toggle = dock(id)->toggleViewAction();
        toggle->setIcon(tinted_svg_icon_file(QStringLiteral(":/stresslab/icons/") +
                                             QString::fromLatin1(icon) + QStringLiteral(".svg")));
        shell_activities.push_back(toggle);
    }
    extensions_->set_hooks(
        {.layout = [this] { return layout_to_json(layout()); },
         .restore_layout =
             [this](const nlohmann::json& json) {
                 const std::optional<Layout> found = layout_from_json(json);
                 return found && restore_layout(*found);
             },
         .show_settings = [this](const std::string& owner) { show_settings_of(owner); },
         .report = [this](const QString& message) { set_prompt(message); }});
    ContributionHosts hosts{
        .window = this,
        .menu_bar = menuBar(),
        .shell_menus = {{"file", file_menu_},
                        {"edit", edit_menu_},
                        {"view", view_menu_},
                        {"help", help_menu_}},
        .stage_bar = find_zone(zone::stage_bar)->bar,
        .activity_bar = find_zone(zone::activity_bar)->bar,
        .shell_activities = std::move(shell_activities),
        .context_toolbar = context_toolbar_,
        .docks = docks_,
        .side_panel = dock(zone::model_tree),
        .bottom_panel = dock(zone::journal),
    };
    contributions_ = std::make_unique<ContributionUi>(workbench_, *commands_, std::move(hosts));

    // Context menus of modules: the model tree adds its own items to them; the 3D view.
    tree_->set_module_menu(
        [this](QWidget* parent) { return contributions_->context_menu("tree", parent); });
    // Commands that take the selected object become available or not with it.
    tree_->set_on_selection([this] { contributions_->refresh(); });
    QWidget* viewport = find_zone(zone::viewport)->placeholder;
    viewport->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(viewport, &QWidget::customContextMenuRequested, this,
            [this, viewport](const QPoint& position) {
                (void)show_context_menu("viewport", viewport, position);
            });

    QList<QAction*> shell_actions{undo_,
                                  redo_,
                                  quit_,
                                  settings_action_,
                                  full_screen_,
                                  about_,
                                  journal_->save_action(),
                                  project_->open_action(),
                                  project_->save_action(),
                                  project_->save_as_action(),
                                  project_->clean_action()};
    for (const Zone& zone : zones_) {
        if (QAction* toggle = zone_action(zone.id)) {
            shell_actions.push_back(toggle);
        }
    }
    palette_ = new CommandPalette(workbench_, *commands_, shell_actions, this);
    connect(contributions_.get(), &ContributionUi::rebuilt, this, [this] {
        if (palette_->isVisible()) {
            palette_->reload();
        }
    });

    for (const std::string_view key : {"history.canUndo", "history.canRedo"}) {
        history_watches_.push_back(workbench_.keys.watch(context::WhenExpression::parse(key),
                                                         [this](bool) { update_history(); }));
    }
    model_changed_ =
        workbench_.bus.on_model_changed([this](const command::ModelChange&) { update_history(); });
    extension_events_ = workbench_.extensions.on_event(
        [this](const extension::ExtensionEvent& event) { on_extension_event(event); });
}

void MainWindow::update_history() {
    const command::CommandBus& bus = workbench_.bus;
    const auto label = [this](std::string_view step) {
        return commands_->title(step).toStdString();
    };
    undo_->setEnabled(bus.can_undo());
    undo_->setText(bus.can_undo() ? text("action.undoStep", {{"step", label(bus.undo_label())}})
                                  : text("action.undo"));
    redo_->setEnabled(bus.can_redo());
    redo_->setText(bus.can_redo() ? text("action.redoStep", {{"step", label(bus.redo_label())}})
                                  : text("action.redo"));
}

void MainWindow::on_extension_event(const extension::ExtensionEvent& event) {
    using Kind = extension::ExtensionEvent::Kind;
    const extension::ExtensionInfo* info = workbench_.extensions.find(event.id);
    if (info == nullptr) {
        return;
    }
    QString message;
    if (event.kind == Kind::crashed && event.crash) {
        message = to_qstring(extension::crash_message(*info, *event.crash, localization_));
    } else if (event.kind == Kind::activation_failed) {
        message = text("problem.activationFailed",
                       {{"module", localization_.manifest_text(info->id(), info->manifest->name)},
                        {"message", event.message}});
    } else {
        return;
    }
    problems_->add(message);
    set_prompt(message);
}

void MainWindow::show_palette() {
    palette_->open_palette();
}

void MainWindow::show_settings() {
    ads::CDockWidget* settings = find_zone(zone::settings)->dock;
    settings->toggleView(true);
    settings->setAsCurrentTab();
    settings->raise();
}

void MainWindow::show_settings_of(const std::string& owner) {
    show_settings();
    (void)settings_->show_section(owner);
}

void MainWindow::show_extensions() {
    ads::CDockWidget* extensions = find_zone(zone::extensions)->dock;
    extensions->toggleView(true);
    extensions->setAsCurrentTab();
    extensions->raise();
}

void MainWindow::check_required_modules() {
    required_->check();
}

QAction* MainWindow::save_journal_action() const noexcept {
    return journal_->save_action();
}

QMenu* MainWindow::show_context_menu(std::string_view location, QWidget* widget,
                                     const QPoint& position) {
    QMenu* menu = contributions_->context_menu(location, this);
    if (menu == nullptr) {
        return nullptr;
    }
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(widget->mapToGlobal(position));
    return menu;
}

void MainWindow::rebuild_workspace_menu() {
    workspace_menu_->clear();
    delete workspace_group_;
    workspace_group_ = new QActionGroup(workspace_menu_);
    const auto add_workspace = [this](const QString& title, const QString& name) {
        QAction* action = workspace_menu_->addAction(title);
        action->setCheckable(true);
        action->setChecked(workspaces_.current() == name);
        action->setActionGroup(workspace_group_);
        connect(action, &QAction::triggered, this, [this, name] {
            if (name.isEmpty()) {
                reset_layout();
            } else {
                (void)open_workspace(name);
            }
        });
    };
    add_workspace(text("workspace.default"), QString());
    for (const QString& name : workspaces_.names()) {
        add_workspace(name, name);
    }
    workspace_menu_->addSeparator();
    QAction* save =
        workspace_menu_->addAction(text("workspace.saveAs"), this, &MainWindow::ask_workspace_name);
    save->setObjectName(QStringLiteral("workspace.saveAs"));
    const QString current = workspaces_.current();
    QAction* remove = workspace_menu_->addAction(
        current.isEmpty() ? text("workspace.delete")
                          : text("workspace.deleteNamed", {{"name", current.toStdString()}}),
        this, [this, current] { (void)delete_workspace(current); });
    remove->setObjectName(QStringLiteral("workspace.delete"));
    remove->setEnabled(!current.isEmpty());
    QAction* reset =
        workspace_menu_->addAction(text("workspace.reset"), this, &MainWindow::reset_layout);
    reset->setObjectName(QStringLiteral("workspace.reset"));
}

void MainWindow::ask_workspace_name() {
    auto* dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("workspaceNameDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(text("workspace.saveAs").remove(QStringLiteral("…")));
    dialog->setLabelText(text("workspace.name"));
    dialog->setTextValue(workspaces_.current());
    dialog->setOkButtonText(text("dialog.ok"));
    dialog->setCancelButtonText(text("dialog.cancel"));
    connect(dialog, &QInputDialog::textValueSelected, this, [this](const QString& value) {
        const QString name = value.trimmed();
        if (!name.isEmpty()) {
            save_workspace(name);
        }
    });
    dialog->open();
}

void MainWindow::retranslate() {
    for (const Zone& zone : zones_) {
        QString title = text(zone.title_key);
        if (zone.id == zone::problems && problems_->count() > 0) {
            title = text("zone.problemsCount", {{"count", std::int64_t{problems_->count()}}});
        }
        zone.widget->setWindowTitle(title);
        if (zone.placeholder != nullptr) {
            zone.placeholder->setText(zone.id == zone::viewport ? title : text("zone.empty"));
        }
    }
    file_menu_->setTitle(text("menu.file"));
    edit_menu_->setTitle(text("menu.edit"));
    view_menu_->setTitle(text("menu.view"));
    palette_action_->setText(text("action.commandPalette"));
    settings_action_->setText(text("action.settings"));
    workspace_menu_->setTitle(text("menu.workspaces"));
    help_menu_->setTitle(text("menu.help"));
    quit_->setText(text("action.quit"));
    full_screen_->setText(text("action.fullScreen"));
    about_->setText(text("action.about", {{"product", std::string(kProductName)}}));
    project_->retranslate();
    rebuild_workspace_menu();
    update_history();
    set_prompt(prompt_text_);
}

QWidget* MainWindow::zone_widget(std::string_view id) const {
    const Zone* zone = find_zone(id);
    return zone ? zone->widget : nullptr;
}

bool MainWindow::is_zone_shown(std::string_view id) const {
    const Zone* zone = find_zone(id);
    if (zone == nullptr) {
        return false;
    }
    if (zone->dock != nullptr) {
        return !zone->dock->isClosed();
    }
    return !zone->bar->isHidden();
}

QAction* MainWindow::zone_action(std::string_view id) const {
    const Zone* zone = find_zone(id);
    if (zone == nullptr || id == zone::viewport) {
        return nullptr;
    }
    return zone->dock ? zone->dock->toggleViewAction() : zone->bar->toggleViewAction();
}

void MainWindow::set_prompt(const QString& text) {
    prompt_text_ = text;
    prompt_->setText(text.isEmpty() ? this->text("prompt.ready") : text);
}

QString MainWindow::prompt() const {
    return prompt_->text();
}

Layout MainWindow::layout() const {
    return {.docks = docks_->saveState(layout_version), .bars = saveState(layout_version)};
}

bool MainWindow::restore_layout(const Layout& layout) {
    if (!docks_->restoreState(layout.docks, layout_version)) {
        return false;
    }
    (void)restoreState(layout.bars, layout_version);
    // A layout saved with other modules enabled says nothing of their panels; their tabs follow
    // their conditions again.
    contributions_->dock_panels_missing_from(layout.docks);
    contributions_->apply_tab_conditions();
    return true;
}

void MainWindow::save_workspace(const QString& name) {
    workspaces_.set(name, layout());
    workspaces_.set_current(name);
    (void)workspaces_.save();
    rebuild_workspace_menu();
}

bool MainWindow::open_workspace(const QString& name) {
    const std::optional<Layout> found = workspaces_.find(name);
    if (!found || !restore_layout(*found)) {
        return false;
    }
    workspaces_.set_current(name);
    rebuild_workspace_menu();
    return true;
}

bool MainWindow::delete_workspace(const QString& name) {
    if (!workspaces_.remove(name)) {
        return false;
    }
    (void)workspaces_.save();
    rebuild_workspace_menu();
    return true;
}

void MainWindow::reset_layout() {
    (void)restore_layout(default_layout_);
    apply_default_sizes();
    workspaces_.set_current(QString());
    rebuild_workspace_menu();
}

AboutDialog* MainWindow::show_about() {
    auto* dialog = new AboutDialog(localization_, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
    return dialog;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!project_->confirm_close()) {
        event->ignore();
        return;
    }
    workspaces_.set_session(layout(), saveGeometry());
    (void)workspaces_.save();
    QMainWindow::closeEvent(event);
}

const MainWindow::Zone* MainWindow::find_zone(std::string_view id) const {
    const auto it = std::find_if(zones_.begin(), zones_.end(),
                                 [id](const Zone& zone) { return zone.id == id; });
    return it == zones_.end() ? nullptr : &*it;
}

QString MainWindow::text(std::string_view key, const l10n::Args& args) const {
    return shell::text(localization_, key, args);
}

}  // namespace fem::shell
