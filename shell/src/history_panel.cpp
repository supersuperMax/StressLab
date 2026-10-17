#include "fem/shell/history_panel.h"

#include "fem/core/log/log.h"
#include "fem/shell/brand.h"
#include "fem/shell/history_controller.h"
#include "fem/shell/history_text.h"
#include "fem/shell/keys.h"
#include "fem/shell/strings.h"
#include "fem/storage/diff.h"
#include "fem/storage/history.h"
#include "fem/storage/session.h"

#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <set>
#include <utility>

namespace fem::shell {

/// A chip before the message of a version: a variant (filled with its colour), a label
/// (outlined), "you are here" (outlined in the accent).
struct HistoryChip {
    QString text;
    QColor color;
    bool filled = false;
};

/// What a row of the graph shows.
struct HistoryRowView {
    std::vector<HistoryChip> chips;
    QString summary;
    /// Author, time, changed sections, results.
    QString details;
    bool automatic = false;
    bool current = false;
};

namespace {

/// The width of a lane of the graph and the radius of a node, in pixels.
constexpr int lane_width = 14;
constexpr int node_radius = 4;
constexpr int padding = 4;

QIcon color_icon(const QColor& color) {
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(1, 1, 10, 10));
    return QIcon(pixmap);
}

}  // namespace

/// Paints a row of the graph: the lines and the node of the version, its chips and message, and
/// a muted second line with the details.
class HistoryGraphDelegate : public QStyledItemDelegate {
public:
    explicit HistoryGraphDelegate(HistoryPanel& panel)
        : QStyledItemDelegate(&panel), panel_(panel) {}

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& /*index*/) const override {
        const int line = option.fontMetrics.lineSpacing();
        return {200, line * 2 + padding * 3};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const int row = index.data(HistoryPanel::row_role).toInt();
        if (row < 0 || static_cast<std::size_t>(row) >= panel_.layout_.rows.size()) {
            return;
        }
        const GraphRow& graph = panel_.layout_.rows[static_cast<std::size_t>(row)];
        const HistoryRowView& view = panel_.views_[static_cast<std::size_t>(row)];
        painter->save();
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = QIcon();
        const QWidget* widget = option.widget;
        QStyle* style = widget != nullptr ? widget->style() : nullptr;
        if (style != nullptr) {
            style->drawPrimitive(QStyle::PE_PanelItemViewItem, &background, painter, widget);
        }
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect rect = option.rect;
        const int line = option.fontMetrics.lineSpacing();
        const int middle = rect.top() + padding + line / 2;
        const auto lane_x = [&rect](int lane) {
            return rect.left() + padding + lane * lane_width + lane_width / 2;
        };
        const auto point_y = [&rect, middle](GraphPoint point) {
            switch (point) {
                case GraphPoint::top:
                    return rect.top();
                case GraphPoint::middle:
                    return middle;
                case GraphPoint::bottom:
                    break;
            }
            return rect.bottom() + 1;
        };
        for (const GraphLine& segment : graph.lines) {
            painter->setPen(QPen(lane_color(segment.color_lane), 2));
            painter->drawLine(QPoint(lane_x(segment.from_lane), point_y(segment.from)),
                              QPoint(lane_x(segment.to_lane), point_y(segment.to)));
        }
        const QColor node = lane_color(graph.lane);
        painter->setPen(QPen(option.palette.color(QPalette::Base), 1.5));
        painter->setBrush(view.automatic ? option.palette.color(QPalette::Base) : node);
        if (view.automatic) {
            painter->setPen(QPen(node, 1.5));
        }
        const int radius = view.current ? node_radius + 2 : node_radius;
        painter->drawEllipse(QPoint(lane_x(graph.lane), middle), radius, radius);
        if (view.current) {
            painter->setPen(QPen(brand::coral, 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(QPoint(lane_x(graph.lane), middle), radius + 2, radius + 2);
        }

        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const QColor text = option.palette.color(
            selected ? QPalette::HighlightedText
                     : (view.automatic ? QPalette::PlaceholderText : QPalette::Text));
        int x = rect.left() + padding * 2 + std::max(panel_.layout_.lanes, 1) * lane_width;
        const int right = rect.right() - padding;
        QFont chip_font = option.font;
        chip_font.setPointSizeF(chip_font.pointSizeF() * 0.85);
        const QFontMetrics chip_metrics(chip_font);
        painter->setFont(chip_font);
        for (const HistoryChip& chip : view.chips) {
            const int width = chip_metrics.horizontalAdvance(chip.text) + 10;
            if (x + width > right) {
                break;
            }
            const QRect box(x, middle - chip_metrics.height() / 2 - 1, width,
                            chip_metrics.height() + 2);
            painter->setPen(chip.filled ? Qt::NoPen : QPen(chip.color, 1));
            painter->setBrush(chip.filled ? QBrush(chip.color) : QBrush(Qt::NoBrush));
            painter->drawRoundedRect(box, 4, 4);
            painter->setPen(chip.filled ? text_color_on(chip.color)
                                        : (selected ? text : chip.color));
            painter->drawText(box, Qt::AlignCenter, chip.text);
            x += width + 4;
        }
        QFont summary_font = option.font;
        summary_font.setBold(view.current);
        painter->setFont(summary_font);
        painter->setPen(text);
        const QFontMetrics summary_metrics(summary_font);
        painter->drawText(
            QRect(x, rect.top() + padding, std::max(0, right - x), line),
            Qt::AlignVCenter | Qt::AlignLeft,
            summary_metrics.elidedText(view.summary, Qt::ElideRight, std::max(0, right - x)));
        const int text_left =
            rect.left() + padding * 2 + std::max(panel_.layout_.lanes, 1) * lane_width;
        painter->setFont(option.font);
        painter->setPen(
            option.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
        painter->drawText(
            QRect(text_left, rect.top() + padding * 2 + line, std::max(0, right - text_left), line),
            Qt::AlignVCenter | Qt::AlignLeft,
            option.fontMetrics.elidedText(view.details, Qt::ElideRight,
                                          std::max(0, right - text_left)));
        painter->restore();
    }

private:
    [[nodiscard]] QColor lane_color(int lane) const {
        const std::vector<std::string>& variants = panel_.layout_.lane_variants;
        if (lane < 0 || static_cast<std::size_t>(lane) >= variants.size() ||
            variants[static_cast<std::size_t>(lane)].empty()) {
            return QColor(0x8A, 0x93, 0x9E);  // Versions no variant reaches.
        }
        return variant_color(variants[static_cast<std::size_t>(lane)]);
    }

    HistoryPanel& panel_;
};

HistoryPanel::HistoryPanel(HistoryController& history, QWidget* parent)
    : QWidget(parent), history_(history), workbench_(history.workbench()) {
    setObjectName(QStringLiteral("historyPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    // The variant to work on, and the actions.
    variants_ = new QComboBox(this);
    variants_->setObjectName(QStringLiteral("history.variants"));
    variants_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(variants_, &QComboBox::activated, this, [this](int index) {
        if (rebuilding_) {
            return;
        }
        const QString name = variants_->itemData(index).toString();
        if (name.isEmpty() || !history_.switch_to(name.toStdString())) {
            rebuild_variants();  // Cancelled or failed: show what is current again.
        }
    });
    layout->addWidget(variants_);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(4);
    const auto add_button = [this, buttons](const char* name) {
        auto* button = new QToolButton(this);
        button->setObjectName(QString::fromLatin1(name));
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        buttons->addWidget(button);
        return button;
    };
    commit_button_ = add_button("history.commit");
    connect(commit_button_, &QToolButton::clicked, this, [this] { (void)start_commit(); });
    compare_button_ = add_button("history.compare");
    connect(compare_button_, &QToolButton::clicked, this, &HistoryPanel::compare_selected);
    buttons->addStretch(1);
    filter_button_ = add_button("history.filters");
    filter_button_->setPopupMode(QToolButton::InstantPopup);
    filter_menu_ = new QMenu(filter_button_);
    filter_button_->setMenu(filter_menu_);
    const auto add_filter = [this](const char* name, bool checked) {
        QAction* action = filter_menu_->addAction(QString());
        action->setObjectName(QString::fromLatin1(name));
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::toggled, this, &HistoryPanel::rebuild);
        return action;
    };
    show_automatic_ = add_filter("history.filter.automatic", true);
    only_results_ = add_filter("history.filter.results", false);
    only_labels_ = add_filter("history.filter.labels", false);
    author_menu_ = filter_menu_->addMenu(QString());
    author_menu_->setObjectName(QStringLiteral("history.filter.author"));
    layout->addLayout(buttons);

    // "Commit version": a short form in the panel, not a dialog.
    commit_pane_ = new QFrame(this);
    commit_pane_->setObjectName(QStringLiteral("history.commitPane"));
    commit_pane_->setFrameShape(QFrame::StyledPanel);
    auto* pane = new QVBoxLayout(commit_pane_);
    pane->setContentsMargins(8, 6, 8, 8);
    commit_title_ = new QLabel(commit_pane_);
    QFont bold = commit_title_->font();
    bold.setBold(true);
    commit_title_->setFont(bold);
    pane->addWidget(commit_title_);
    message_ = new QLineEdit(commit_pane_);
    message_->setObjectName(QStringLiteral("history.message"));
    connect(message_, &QLineEdit::returnPressed, this, [this] { (void)finish_commit(); });
    pane->addWidget(message_);
    changes_title_ = new QLabel(commit_pane_);
    changes_title_->setForegroundRole(QPalette::PlaceholderText);
    pane->addWidget(changes_title_);
    changes_ = new QListWidget(commit_pane_);
    changes_->setObjectName(QStringLiteral("history.changes"));
    changes_->setWordWrap(true);
    changes_->setMaximumHeight(140);
    changes_->setSelectionMode(QAbstractItemView::NoSelection);
    pane->addWidget(changes_);
    auto* pane_buttons = new QHBoxLayout;
    pane_buttons->addStretch(1);
    commit_cancel_ = new QPushButton(commit_pane_);
    commit_cancel_->setObjectName(QStringLiteral("history.commitCancel"));
    connect(commit_cancel_, &QPushButton::clicked, this, &HistoryPanel::cancel_commit);
    pane_buttons->addWidget(commit_cancel_);
    commit_ok_ = new QPushButton(commit_pane_);
    commit_ok_->setObjectName(QStringLiteral("history.commitOk"));
    commit_ok_->setDefault(true);
    connect(commit_ok_, &QPushButton::clicked, this, [this] { (void)finish_commit(); });
    pane_buttons->addWidget(commit_ok_);
    pane->addLayout(pane_buttons);
    commit_pane_->hide();
    layout->addWidget(commit_pane_);

    // The graph.
    graph_ = new QListWidget(this);
    graph_->setObjectName(QStringLiteral("history.graph"));
    graph_->setItemDelegate(new HistoryGraphDelegate(*this));
    graph_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    graph_->setUniformItemSizes(true);
    graph_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(graph_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        const std::string version = item->data(version_role).toString().toStdString();
        if (version != history_.state().version) {
            (void)history_.view(version);
        }
    });
    connect(graph_, &QListWidget::customContextMenuRequested, this, &HistoryPanel::show_menu);
    layout->addWidget(graph_, 1);
    empty_ = new QLabel(this);
    empty_->setObjectName(QStringLiteral("history.empty"));
    empty_->setWordWrap(true);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setForegroundRole(QPalette::PlaceholderText);
    empty_->setMargin(12);
    layout->addWidget(empty_, 1);

    // Changes put aside.
    stashes_ = new QFrame(this);
    stashes_->setObjectName(QStringLiteral("history.stashes"));
    auto* stash_layout = new QVBoxLayout(stashes_);
    stash_layout->setContentsMargins(0, 0, 0, 0);
    stashes_title_ = new QLabel(stashes_);
    stashes_title_->setFont(bold);
    stash_layout->addWidget(stashes_title_);
    stash_list_ = new QListWidget(stashes_);
    stash_list_->setMaximumHeight(90);
    stash_layout->addWidget(stash_list_);
    auto* stash_buttons = new QHBoxLayout;
    stash_buttons->addStretch(1);
    stash_drop_ = new QPushButton(stashes_);
    stash_drop_->setObjectName(QStringLiteral("history.stashDrop"));
    stash_pop_ = new QPushButton(stashes_);
    stash_pop_->setObjectName(QStringLiteral("history.stashPop"));
    const auto stash_index = [this] {
        const int row = stash_list_->currentRow();
        return row < 0 ? std::size_t{0} : static_cast<std::size_t>(row);
    };
    connect(stash_pop_, &QPushButton::clicked, this,
            [this, stash_index] { (void)history_.stash_pop(stash_index()); });
    connect(stash_drop_, &QPushButton::clicked, this,
            [this, stash_index] { (void)history_.stash_drop(stash_index()); });
    stash_buttons->addWidget(stash_drop_);
    stash_buttons->addWidget(stash_pop_);
    stash_layout->addLayout(stash_buttons);
    layout->addWidget(stashes_);

    commit_action_ = new QAction(this);
    commit_action_->setObjectName(QStringLiteral("history.commitVersion"));
    commit_action_->setShortcut(key_sequence("Mod+Shift+S"));
    connect(commit_action_, &QAction::triggered, this, [this] { (void)start_commit(); });

    connect(&history_, &HistoryController::changed, this, &HistoryPanel::rebuild);
    connect(&history_, &HistoryController::terms_changed, this, &HistoryPanel::retranslate);
    language_ =
        workbench_.localization.on_language_changed([this](const std::string&) { retranslate(); });
    retranslate();
}

HistoryPanel::~HistoryPanel() = default;

// --- Commit --------------------------------------------------------------------------------------

bool HistoryPanel::start_commit() {
    const HistoryState& state = history_.state();
    if (!state.has_folder || state.viewing()) {
        return false;
    }
    changes_->clear();
    const std::vector<std::string> changes = history_.pending_changes();
    for (const std::string& change : changes) {
        changes_->addItem(to_qstring(change));
    }
    if (changes.empty()) {
        changes_->addItem(text(state.version ? "history.noChanges" : "history.firstVersion"));
    }
    message_->clear();
    message_->setPlaceholderText(history_.automatic_message());
    commit_pane_->show();
    message_->setFocus();
    return true;
}

bool HistoryPanel::finish_commit() {
    if (!is_committing() || !history_.commit(message_->text())) {
        return false;
    }
    commit_pane_->hide();
    return true;
}

void HistoryPanel::cancel_commit() {
    commit_pane_->hide();
}

bool HistoryPanel::is_committing() const {
    return !commit_pane_->isHidden();
}

// --- The graph -----------------------------------------------------------------------------------

std::string HistoryPanel::version_at(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= laid_out_.size()) {
        return {};
    }
    return laid_out_[static_cast<std::size_t>(row)];
}

int HistoryPanel::row_of(const std::string& version) const {
    const auto it = std::find(laid_out_.begin(), laid_out_.end(), version);
    return it == laid_out_.end() ? -1 : static_cast<int>(it - laid_out_.begin());
}

void HistoryPanel::set_show_automatic(bool show) {
    show_automatic_->setChecked(show);
}

void HistoryPanel::set_only_with_results(bool only) {
    only_results_->setChecked(only);
}

void HistoryPanel::set_only_with_labels(bool only) {
    only_labels_->setChecked(only);
}

void HistoryPanel::set_author(const QString& name) {
    author_ = name;
    rebuild();
}

QString HistoryPanel::changed_sections(const std::string& version) {
    if (const auto found = sections_.find(version); found != sections_.end()) {
        return found->second;
    }
    QString result;
    const storage::VersionInfo* info = history_.state().find(version);
    if (info != nullptr && !info->parents.empty() && workbench_.project != nullptr) {
        try {
            const storage::ModelDiff diff =
                workbench_.project->history().diff(info->parents.front(), version);
            QStringList sections;
            for (const storage::DiffRow& row :
                 storage::describe_rows(diff, {.localization = workbench_.localization,
                                               .registry = &workbench_.extensions.registry()})) {
                const QString section = to_qstring(row.section);
                if (!sections.contains(section)) {
                    sections.push_back(section);
                }
            }
            result = sections.join(QStringLiteral(", "));
        } catch (const std::exception& error) {
            log::logger("shell")->warn("the changes of version {} cannot be read: {}", version,
                                       error.what());
        }
    }
    sections_.emplace(version, result);
    return result;
}

void HistoryPanel::rebuild() {
    rebuilding_ = true;
    const HistoryState& state = history_.state();
    rebuild_variants();
    rebuild_stashes();
    rebuild_author_menu();

    std::map<std::string, std::vector<std::string>> variants_at;
    for (const auto& [name, id] : state.branches) {
        variants_at[id].push_back(name);
    }
    std::map<std::string, std::vector<std::string>> labels_at;
    for (const auto& [name, id] : state.tags) {
        labels_at[id].push_back(name);
    }
    std::vector<GraphVersion> versions;
    versions.reserve(state.versions.size());
    for (const storage::VersionInfo& info : state.versions) {
        versions.push_back({.id = info.id, .parents = info.parents});
    }
    // The main variant has the first lane, the others follow by name.
    std::vector<GraphHead> heads;
    if (const auto main = state.branches.find(std::string(storage::main_branch));
        main != state.branches.end()) {
        heads.push_back({.name = main->first, .version = main->second});
    }
    for (const auto& [name, id] : state.branches) {
        if (name != storage::main_branch) {
            heads.push_back({.name = name, .version = id});
        }
    }
    const auto shown = [&](std::size_t index) {
        const storage::VersionInfo& info = state.versions[index];
        if (state.version == info.id) {
            return true;  // Where the model is: always.
        }
        if (!show_automatic_->isChecked() && info.kind == storage::VersionKind::automatic) {
            return false;
        }
        if (only_results_->isChecked() && history_.results_of(info.id).empty()) {
            return false;
        }
        if (only_labels_->isChecked() && !labels_at.contains(info.id)) {
            return false;
        }
        return author_.isEmpty() || to_qstring(info.author.name) == author_;
    };
    layout_ = layout_graph(versions, heads, shown);

    std::set<std::string> selected;
    for (const QListWidgetItem* item : graph_->selectedItems()) {
        selected.insert(item->data(version_role).toString().toStdString());
    }
    graph_->clear();
    views_.clear();
    laid_out_.clear();
    const QColor label_color = palette().color(QPalette::Text);
    for (std::size_t row = 0; row < layout_.rows.size(); ++row) {
        const storage::VersionInfo& info = state.versions[layout_.rows[row].version];
        HistoryRowView view;
        view.current = state.version == info.id;
        view.automatic = info.kind == storage::VersionKind::automatic;
        view.summary = to_qstring(info.summary.empty() ? info.message : info.summary);
        QStringList outdated;
        if (const auto names = variants_at.find(info.id); names != variants_at.end()) {
            for (const std::string& name : names->second) {
                view.chips.push_back({.text = history_.variant_title(name),
                                      .color = variant_color(name),
                                      .filled = true});
                if (history_.results_outdated(name)) {
                    outdated.push_back(history_.variant_title(name));
                }
            }
        }
        if (view.current) {
            view.chips.push_back(
                {.text = text("history.youAreHere"), .color = brand::coral, .filled = false});
        }
        if (const auto names = labels_at.find(info.id); names != labels_at.end()) {
            for (const std::string& name : names->second) {
                view.chips.push_back(
                    {.text = to_qstring(name), .color = label_color, .filled = false});
            }
        }
        QStringList details{to_qstring(info.author.name), history_.time(info.time)};
        if (const QString sections = changed_sections(info.id); !sections.isEmpty()) {
            details.push_back(sections);
        }
        const std::vector<const storage::ResultInfo*> results = history_.results_of(info.id);
        if (!results.empty()) {
            const bool pinned = std::any_of(results.begin(), results.end(),
                                            [](const auto* result) { return result->pinned; });
            details.push_back(text(pinned ? "history.resultsPinned" : "history.results"));
        }
        if (!outdated.isEmpty()) {
            details.push_back(text("history.resultsOutdated"));
        }
        view.details = details.join(QStringLiteral(" · "));

        auto* item = new QListWidgetItem(view.summary, graph_);
        item->setData(version_role, to_qstring(info.id));
        item->setData(row_role, static_cast<int>(row));
        item->setToolTip(
            to_qstring(info.message) + QStringLiteral("\n\n") + to_qstring(info.author.name) +
            QStringLiteral(" <") + to_qstring(info.author.email) + QStringLiteral(">\n") +
            history_.time(info.time) + QStringLiteral("\n") + to_qstring(info.id.substr(0, 10)));
        item->setSelected(selected.contains(info.id));
        laid_out_.push_back(info.id);
        views_.push_back(std::move(view));
    }

    const bool empty = layout_.rows.empty();
    graph_->setVisible(!empty);
    empty_->setVisible(empty);
    empty_->setText(!state.has_folder        ? text("history.noFolder")
                    : state.versions.empty() ? text("history.noVersions")
                                             : text("history.nothingShown"));
    const bool can_commit = state.has_folder && !state.viewing();
    commit_button_->setEnabled(can_commit);
    commit_action_->setEnabled(can_commit);
    compare_button_->setEnabled(state.has_folder && !state.versions.empty());
    if (!can_commit) {
        commit_pane_->hide();
    }
    rebuilding_ = false;
}

void HistoryPanel::rebuild_variants() {
    const bool was = rebuilding_;
    rebuilding_ = true;
    const HistoryState& state = history_.state();
    variants_->clear();
    if (state.viewing()) {
        variants_->addItem(history_.current_title(), QString());
    }
    std::vector<std::string> names;
    if (state.branches.contains(std::string(storage::main_branch))) {
        names.emplace_back(storage::main_branch);
    }
    for (const auto& [name, id] : state.branches) {
        if (name != storage::main_branch) {
            names.push_back(name);
        }
    }
    if (names.empty() && state.has_folder && !state.viewing()) {
        // No version yet: the variant HEAD is on.
        names.push_back(state.branch.value_or(std::string(storage::main_branch)));
    }
    for (const std::string& name : names) {
        variants_->addItem(color_icon(variant_color(name)), history_.variant_title(name),
                           to_qstring(name));
        if (state.branch == name) {
            variants_->setCurrentIndex(variants_->count() - 1);
        }
    }
    if (state.viewing()) {
        variants_->setCurrentIndex(0);
    }
    variants_->setEnabled(state.has_folder && variants_->count() > 0);
    rebuilding_ = was;
}

void HistoryPanel::rebuild_stashes() {
    const HistoryState& state = history_.state();
    stash_list_->clear();
    for (const storage::VersionInfo& stash : state.stashes) {
        QString line =
            to_qstring(stash.summary) + QStringLiteral(" · ") + history_.time(stash.time);
        if (!stash.branch.empty()) {
            line += QStringLiteral(" · ") + history_.variant_title(stash.branch);
        }
        stash_list_->addItem(line);
    }
    if (stash_list_->count() > 0) {
        stash_list_->setCurrentRow(0);
    }
    stashes_->setVisible(!state.stashes.empty());
}

void HistoryPanel::rebuild_author_menu() {
    author_menu_->clear();
    auto* group = new QActionGroup(author_menu_);
    std::set<QString> names;
    for (const storage::VersionInfo& info : history_.state().versions) {
        names.insert(to_qstring(info.author.name));
    }
    if (!author_.isEmpty()) {
        names.insert(author_);
    }
    const auto add = [this, group](const QString& title, const QString& name) {
        QAction* action = author_menu_->addAction(title);
        action->setCheckable(true);
        action->setChecked(author_ == name);
        action->setActionGroup(group);
        connect(action, &QAction::triggered, this, [this, name] { set_author(name); });
    };
    add(text("history.filter.everyone"), QString());
    for (const QString& name : names) {
        add(name, name);
    }
}

// --- The context menu and comparing --------------------------------------------------------------

void HistoryPanel::show_menu(const QPoint& position) {
    QListWidgetItem* item = graph_->itemAt(position);
    if (item == nullptr) {
        return;
    }
    QMenu* menu = version_menu(item->data(version_role).toString().toStdString(), this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(graph_->viewport()->mapToGlobal(position));
}

QMenu* HistoryPanel::version_menu(const std::string& version, QWidget* parent) {
    const HistoryState& state = history_.state();
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("history.menu"));
    const auto add = [this, menu](const char* name, const QString& title, auto slot) {
        QAction* action = menu->addAction(title, this, slot);
        action->setObjectName(QString::fromLatin1(name));
        return action;
    };
    const bool current = state.version == version;
    add("history.menu.view", text("history.menu.view"), [this, version] {
        (void)history_.view(version);
    })->setEnabled(!current);
    for (const auto& [name, id] : state.branches) {
        if (id == version && state.branch != name) {
            add("history.menu.switch",
                text("history.menu.switch",
                     {{"variant", history_.variant_title(name).toStdString()}}),
                [this, name = name] { (void)history_.switch_to(name); });
        }
    }
    add("history.menu.newVariant", text("history.menu.newVariant"),
        [this, version] { (void)history_.make_variant(version); });
    add("history.menu.label", text("history.menu.label"),
        [this, version] { (void)history_.tag(version); });
    add("history.menu.restore", text("history.menu.restore"), [this, version] {
        (void)history_.restore(version);
    })->setEnabled(!current || state.viewing());
    menu->addSeparator();
    add("history.menu.compareCurrent", text("history.menu.compareCurrent"),
        [this, version] { Q_EMIT compare_requested(to_qstring(version), QString()); });
    add("history.menu.mark", text("history.menu.mark"),
        [this, version] { mark_for_comparison(version); });
    if (!marked_.empty() && marked_ != version) {
        add("history.menu.compareMarked", text("history.menu.compareMarked"), [this, version] {
            // The older of the two first, as in the graph.
            const bool marked_older = row_of(marked_) > row_of(version);
            Q_EMIT compare_requested(to_qstring(marked_older ? marked_ : version),
                                     to_qstring(marked_older ? version : marked_));
        });
    }
    return menu;
}

void HistoryPanel::mark_for_comparison(const std::string& version) {
    marked_ = version;
}

void HistoryPanel::compare_selected() {
    std::vector<int> rows;
    for (const QListWidgetItem* item : graph_->selectedItems()) {
        rows.push_back(graph_->row(item));
    }
    std::sort(rows.begin(), rows.end());
    if (rows.size() >= 2) {
        // Lower rows are older.
        Q_EMIT compare_requested(to_qstring(version_at(rows.back())),
                                 to_qstring(version_at(rows.front())));
        return;
    }
    if (rows.size() == 1) {
        Q_EMIT compare_requested(to_qstring(version_at(rows.front())), QString());
        return;
    }
    if (const std::optional<std::string>& version = history_.state().version) {
        Q_EMIT compare_requested(to_qstring(*version), QString());
    }
}

// --- Texts ---------------------------------------------------------------------------------------

void HistoryPanel::retranslate() {
    commit_button_->setText(text("history.commitButton"));
    commit_button_->setToolTip(text("history.commitTip"));
    compare_button_->setText(text("history.compareButton"));
    compare_button_->setToolTip(text("history.compareTip"));
    filter_button_->setText(text("history.filterButton"));
    show_automatic_->setText(text("history.filter.automatic"));
    only_results_->setText(text("history.filter.results"));
    only_labels_->setText(text("history.filter.labels"));
    author_menu_->setTitle(text("history.filter.author"));
    commit_action_->setText(text("action.commitVersion"));
    commit_title_->setText(text("history.commitTitle"));
    changes_title_->setText(text("history.changesTitle"));
    commit_ok_->setText(text("history.commitOk"));
    commit_cancel_->setText(text("dialog.cancel"));
    stashes_title_->setText(text("history.stashesTitle"));
    stash_pop_->setText(text("history.stashPop"));
    stash_drop_->setText(text("history.stashDrop"));
    sections_.clear();
    rebuild();
}

QString HistoryPanel::text(std::string_view key, const l10n::Args& args) const {
    return history_.text(key, args);
}

}  // namespace fem::shell
