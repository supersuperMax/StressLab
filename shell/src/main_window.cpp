#include "fem/shell/main_window.h"

#include "fem/core/product.h"
#include "fem/shell/about_dialog.h"
#include "fem/shell/brand.h"
#include "fem/shell/keys.h"
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
#include <algorithm>
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

MainWindow::MainWindow(l10n::Localization& localization, WorkspaceStore& workspaces,
                       QWidget* parent)
    : QMainWindow(parent), localization_(localization), workspaces_(workspaces) {
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(to_qstring(kProductName));
    setWindowIcon(brand::application_icon());
    resize(default_size);

    build_zones();
    build_menus();
    retranslate();
    apply_default_sizes();
    default_layout_ = layout();

    if (workspaces_.session() && restore_layout(*workspaces_.session())) {
        (void)restoreGeometry(workspaces_.geometry());
    }
    language_ = localization_.on_language_changed([this](const std::string&) { retranslate(); });
}

MainWindow::~MainWindow() = default;

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

    // The 3D view: the central area, always there. A stub until the viewport module (task 3.7).
    QLabel* viewport_stub = make_placeholder(nullptr);
    QFont font = viewport_stub->font();
    font.setPointSizeF(font.pointSizeF() * 1.6);
    viewport_stub->setFont(font);
    ads::CDockWidget* viewport = add_dock(zone::viewport, "zone.viewport", viewport_stub);
    zones_.back().placeholder = viewport_stub;
    viewport->setFeature(ads::CDockWidget::NoTab, true);
    ads::CDockAreaWidget* central = docks_->setCentralWidget(viewport);

    docks_->addDockWidget(ads::LeftDockWidgetArea,
                          add_empty_dock(zone::model_tree, "zone.modelTree"), central);
    docks_->addDockWidget(ads::RightDockWidgetArea,
                          add_empty_dock(zone::properties, "zone.properties"), central);

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
    ads::CDockWidget* journal = add_empty_dock(zone::journal, "zone.journal");
    ads::CDockAreaWidget* bottom = docks_->addDockWidget(ads::BottomDockWidgetArea, journal);
    for (const auto& [id, key] :
         {std::pair{zone::python_console, "zone.console"}, std::pair{zone::jobs, "zone.jobs"},
          std::pair{zone::problems, "zone.problems"}}) {
        docks_->addDockWidgetTabToArea(add_empty_dock(id, key), bottom);
    }
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
    quit_ = file_menu_->addAction(QString(), this, &QWidget::close);
    quit_->setObjectName(QStringLiteral("app.quit"));
    // On macOS the item goes to the application menu, as Cmd+Q.
    quit_->setMenuRole(QAction::QuitRole);
    quit_->setShortcut(key_sequence("Mod+Q"));

    view_menu_ = menuBar()->addMenu(QString());
    view_menu_->setObjectName(QStringLiteral("menu.view"));
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
        const QString title = text(zone.title_key);
        zone.widget->setWindowTitle(title);
        if (zone.placeholder != nullptr) {
            zone.placeholder->setText(zone.id == zone::viewport ? title : text("zone.empty"));
        }
    }
    file_menu_->setTitle(text("menu.file"));
    view_menu_->setTitle(text("menu.view"));
    workspace_menu_->setTitle(text("menu.workspaces"));
    help_menu_->setTitle(text("menu.help"));
    quit_->setText(text("action.quit"));
    full_screen_->setText(text("action.fullScreen"));
    about_->setText(text("action.about", {{"product", std::string(kProductName)}}));
    rebuild_workspace_menu();
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
