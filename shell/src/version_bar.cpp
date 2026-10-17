#include "fem/shell/version_bar.h"

#include "fem/shell/history_controller.h"
#include "fem/shell/history_text.h"
#include "fem/shell/strings.h"
#include "fem/storage/repository.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <string>

namespace fem::shell {

// --- VersionBar ----------------------------------------------------------------------------------

VersionBar::VersionBar(HistoryController& history, QWidget* parent)
    : QFrame(parent), history_(history) {
    setObjectName(QStringLiteral("versionBar"));
    setFrameShape(QFrame::StyledPanel);
    setBackgroundRole(QPalette::AlternateBase);
    setAutoFillBackground(true);
    text_ = new QLabel(this);
    text_->setObjectName(QStringLiteral("versionBar.text"));
    text_->setTextFormat(Qt::PlainText);
    // One line, shortened to the room the buttons leave.
    text_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    text_->setMinimumWidth(0);
    text_->installEventFilter(this);
    back_ = new QPushButton(this);
    back_->setObjectName(QStringLiteral("versionBar.back"));
    connect(back_, &QPushButton::clicked, this, [this] { (void)history_.return_to_current(); });
    new_variant_ = new QPushButton(this);
    new_variant_->setObjectName(QStringLiteral("versionBar.newVariant"));
    connect(new_variant_, &QPushButton::clicked, this, [this] {
        (void)history_.make_variant(history_.state().version.value_or(std::string()));
    });
    restore_ = new QPushButton(this);
    restore_->setObjectName(QStringLiteral("versionBar.restore"));
    connect(restore_, &QPushButton::clicked, this, [this] {
        if (const std::optional<std::string> version = history_.state().version) {
            (void)history_.restore(*version);
        }
    });
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 6, 4);
    layout->addWidget(text_, 1);
    layout->addWidget(back_);
    layout->addWidget(new_variant_);
    layout->addWidget(restore_);
    connect(&history_, &HistoryController::changed, this, &VersionBar::update_state);
    update_state();
}

VersionBar::~VersionBar() = default;

QString VersionBar::message() const {
    return message_;
}

bool VersionBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == text_ && event->type() == QEvent::Resize) {
        fit_text();
    }
    return QFrame::eventFilter(watched, event);
}

void VersionBar::fit_text() {
    text_->setText(text_->fontMetrics().elidedText(message_, Qt::ElideRight, text_->width()));
    text_->setToolTip(message_);
}

void VersionBar::update_state() {
    const HistoryState& state = history_.state();
    setVisible(state.viewing());
    if (!state.viewing()) {
        return;
    }
    const storage::VersionInfo* info = state.find(*state.version);
    message_ = history_.text(
        "versionBar.text",
        {{"time", info != nullptr ? history_.time(info->time).toStdString() : std::string()},
         {"message", info != nullptr ? info->summary : *state.version}});
    fit_text();
    back_->setText(history_.text("versionBar.back"));
    new_variant_->setText(history_.text("versionBar.newVariant"));
    restore_->setText(history_.text("versionBar.restore"));
}

// --- VariantSwitcher -----------------------------------------------------------------------------

VariantSwitcher::VariantSwitcher(HistoryController& history, QWidget* parent)
    : QToolButton(parent), history_(history) {
    setObjectName(QStringLiteral("variantSwitcher"));
    setPopupMode(QToolButton::InstantPopup);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    menu_ = new QMenu(this);
    setMenu(menu_);
    connect(menu_, &QMenu::aboutToShow, this, &VariantSwitcher::rebuild_menu);
    connect(&history_, &HistoryController::changed, this, &VariantSwitcher::update_state);
    update_state();
}

VariantSwitcher::~VariantSwitcher() = default;

void VariantSwitcher::update_state() {
    const HistoryState& state = history_.state();
    setText(history_.current_title());
    setToolTip(
        history_.text(state.viewing() ? "variantSwitcher.viewingTip" : "variantSwitcher.tip"));
    // The colour of the variant; grey while a version is viewed on its own.
    const QColor color =
        state.viewing() ? QColor(0x8A, 0x93, 0x9E)
                        : variant_color(state.branch.value_or(std::string(storage::main_branch)));
    setStyleSheet(QStringLiteral("QToolButton#variantSwitcher { background: %1; color: %2; "
                                 "border-radius: 4px; padding: 2px 10px; font-weight: bold; }")
                      .arg(color.name(), text_color_on(color).name()));
}

void VariantSwitcher::rebuild_menu() {
    menu_->clear();
    const HistoryState& state = history_.state();
    std::vector<std::string> names;
    if (state.branches.contains(std::string(storage::main_branch))) {
        names.emplace_back(storage::main_branch);
    }
    for (const auto& [name, id] : state.branches) {
        if (name != storage::main_branch) {
            names.push_back(name);
        }
    }
    for (const std::string& name : names) {
        QAction* action = menu_->addAction(history_.variant_title(name));
        action->setObjectName(QStringLiteral("variantSwitcher.variant"));
        action->setData(to_qstring(name));
        action->setCheckable(true);
        action->setChecked(state.branch == name);
        connect(action, &QAction::triggered, this,
                [this, name] { (void)history_.switch_to(name); });
    }
    if (!names.empty()) {
        menu_->addSeparator();
    }
    QAction* add = menu_->addAction(history_.text("variantSwitcher.newVariant"), this,
                                    [this] { (void)history_.make_variant({}); });
    add->setObjectName(QStringLiteral("variantSwitcher.newVariant"));
    add->setEnabled(state.version.has_value());
    QAction* open = menu_->addAction(history_.text("variantSwitcher.history"), this,
                                     [this] { Q_EMIT history_requested(); });
    open->setObjectName(QStringLiteral("variantSwitcher.history"));
}

}  // namespace fem::shell
