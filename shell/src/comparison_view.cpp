#include "fem/shell/comparison_view.h"

#include "fem/core/log/log.h"
#include "fem/core/model/object.h"
#include "fem/core/model/quantity.h"
#include "fem/shell/history_controller.h"
#include "fem/shell/strings.h"
#include "fem/storage/history.h"
#include "fem/storage/session.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace fem::shell {

namespace {

/// Tints of "was" and "is" that read on both themes.
const QColor before_tint(235, 87, 87, 45);
const QColor after_tint(39, 174, 96, 45);

void tint(QTreeWidgetItem* item, int column, const QColor& color) {
    item->setBackground(column, color);
}

/// The indicators of the result a version stands for: of its result used last that has some.
storage::ResultIndicators indicators_of(const HistoryController& history,
                                        const std::optional<std::string>& version) {
    if (!version) {
        return {};
    }
    for (const storage::ResultInfo* result : history.results_of(*version)) {
        if (!result->indicators.empty()) {
            return result->indicators;
        }
    }
    return {};
}

}  // namespace

ComparisonView::ComparisonView(HistoryController& history, QWidget* parent)
    : QWidget(parent), history_(history) {
    setObjectName(QStringLiteral("comparison"));
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 10);

    auto* header = new QHBoxLayout;
    title_ = new QLabel(this);
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);
    header->addWidget(title_);
    before_ = new QComboBox(this);
    before_->setObjectName(QStringLiteral("comparison.before"));
    after_ = new QComboBox(this);
    after_->setObjectName(QStringLiteral("comparison.after"));
    for (QComboBox* selector : {before_, after_}) {
        // The two share the width of the header; long titles are cut.
        selector->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        selector->setMinimumContentsLength(16);
        selector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(selector, &QComboBox::activated, this, [this](int) { refresh(); });
    }
    header->addWidget(before_, 1);
    header->addWidget(new QLabel(QStringLiteral("→"), this));
    header->addWidget(after_, 1);
    swap_ = new QToolButton(this);
    swap_->setObjectName(QStringLiteral("comparison.swap"));
    swap_->setText(QStringLiteral("⇄"));
    connect(swap_, &QToolButton::clicked, this, [this] { compare(after(), before()); });
    header->addWidget(swap_);
    close_ = new QToolButton(this);
    close_->setObjectName(QStringLiteral("comparison.close"));
    connect(close_, &QToolButton::clicked, this, &ComparisonView::closed);
    header->addWidget(close_);
    layout->addLayout(header);

    differences_ = new QTreeWidget(this);
    differences_->setObjectName(QStringLiteral("comparison.differences"));
    differences_->setColumnCount(3);
    differences_->setAlternatingRowColors(false);
    differences_->setUniformRowHeights(true);
    // The object and field as wide as they are, A and B sharing the rest.
    differences_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    differences_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    differences_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(differences_, &QTreeWidget::itemChanged, this, [this] {
        if (!rebuilding_) {
            update_restore();
        }
    });
    layout->addWidget(differences_, 3);
    same_ = new QLabel(this);
    same_->setObjectName(QStringLiteral("comparison.same"));
    same_->setAlignment(Qt::AlignCenter);
    same_->setForegroundRole(QPalette::PlaceholderText);
    same_->setWordWrap(true);
    layout->addWidget(same_, 3);

    auto* restore_row = new QHBoxLayout;
    restore_row->addStretch(1);
    restore_ = new QPushButton(this);
    restore_->setObjectName(QStringLiteral("comparison.restore"));
    connect(restore_, &QPushButton::clicked, this, [this] { (void)restore_checked(); });
    restore_row->addWidget(restore_);
    layout->addLayout(restore_row);

    indicators_title_ = new QLabel(this);
    indicators_title_->setFont(bold);
    layout->addWidget(indicators_title_);
    indicators_ = new QTableWidget(this);
    indicators_->setObjectName(QStringLiteral("comparison.indicators"));
    indicators_->setColumnCount(4);
    indicators_->verticalHeader()->hide();
    indicators_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    QHeaderView* columns = indicators_->horizontalHeader();
    columns->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    columns->setSectionResizeMode(1, QHeaderView::Stretch);
    columns->setSectionResizeMode(2, QHeaderView::Stretch);
    columns->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(indicators_, 1);
    no_indicators_ = new QLabel(this);
    no_indicators_->setObjectName(QStringLiteral("comparison.noIndicators"));
    no_indicators_->setForegroundRole(QPalette::PlaceholderText);
    no_indicators_->setWordWrap(true);
    layout->addWidget(no_indicators_);

    connect(&history_, &HistoryController::changed, this, [this] {
        rebuild_selectors();
        if (isVisible()) {
            refresh();
        }
    });
    model_changed_ = history_.workbench().bus.on_model_changed([this](const command::ModelChange&) {
        // The current model changed: so did a comparison with it.
        if (isVisible() && (before().isEmpty() || after().isEmpty())) {
            refresh();
        }
    });
    connect(&history_, &HistoryController::terms_changed, this, &ComparisonView::retranslate);
    language_ = history_.workbench().localization.on_language_changed(
        [this](const std::string&) { retranslate(); });
    retranslate();
}

ComparisonView::~ComparisonView() = default;

QString ComparisonView::before() const {
    return before_->currentData().toString();
}

QString ComparisonView::after() const {
    return after_->currentData().toString();
}

void ComparisonView::compare(const QString& before, const QString& after) {
    QString earlier = before;
    QString later = after;
    if (earlier.isEmpty() && !later.isEmpty()) {
        std::swap(earlier, later);
    }
    const HistoryState& state = history_.state();
    for (const QString& picked : {earlier, later}) {
        const std::string id = picked.toStdString();
        if (!id.empty() && !state.branches.contains(id) && !state.tags.contains(id) &&
            std::find(picked_.begin(), picked_.end(), id) == picked_.end()) {
            picked_.push_back(id);
        }
    }
    rebuild_selectors();
    rebuilding_ = true;
    before_->setCurrentIndex(std::max(0, before_->findData(earlier)));
    after_->setCurrentIndex(std::max(0, after_->findData(later)));
    rebuilding_ = false;
    refresh();
}

void ComparisonView::rebuild_selectors() {
    const bool was = rebuilding_;
    rebuilding_ = true;
    const HistoryState& state = history_.state();
    for (QComboBox* selector : {before_, after_}) {
        const QString current = selector->currentData().toString();
        selector->clear();
        selector->addItem(text("comparison.currentModel"), QString());
        std::vector<std::string> variants;
        if (state.branches.contains(std::string(storage::main_branch))) {
            variants.emplace_back(storage::main_branch);
        }
        for (const auto& [name, id] : state.branches) {
            if (name != storage::main_branch) {
                variants.push_back(name);
            }
        }
        for (const std::string& name : variants) {
            selector->addItem(state_title(to_qstring(name)), to_qstring(name));
        }
        for (const auto& [name, id] : state.tags) {
            selector->addItem(state_title(to_qstring(name)), to_qstring(name));
        }
        for (const std::string& id : picked_) {
            if (state.find(id) != nullptr) {
                selector->addItem(state_title(to_qstring(id)), to_qstring(id));
            }
        }
        selector->setCurrentIndex(std::max(0, selector->findData(current)));
    }
    rebuilding_ = was;
}

void ComparisonView::refresh() {
    if (rebuilding_) {
        return;
    }
    if (before().isEmpty() && !after().isEmpty()) {
        compare(before(), after());  // A is the earlier state: swap them.
        return;
    }
    differences_->setHeaderLabels(
        {text("comparison.field"), state_title(before()), state_title(after())});
    rows_.clear();
    storage::ModelDiff diff;
    QString failure;
    Workbench& workbench = history_.workbench();
    if (workbench.project != nullptr && !before().isEmpty()) {
        try {
            const std::string to = after().toStdString();
            diff = workbench.project->history().diff(
                before().toStdString(),
                to.empty() ? std::nullopt : std::optional<std::string_view>(to));
        } catch (const std::exception& error) {
            log::logger("shell")->error("the comparison failed: {}", error.what());
            failure = to_qstring(error.what());
        }
    }
    fill_differences(diff);
    fill_indicators(diff);
    if (!failure.isEmpty()) {
        same_->setText(text("comparison.failed", {{"reason", failure.toStdString()}}));
    }
    update_restore();
}

void ComparisonView::fill_differences(const storage::ModelDiff& diff) {
    rebuilding_ = true;
    differences_->clear();
    Workbench& workbench = history_.workbench();
    rows_ = storage::describe_rows(diff, {.localization = workbench.localization,
                                          .registry = &workbench.extensions.registry()});
    const bool checkable = after().isEmpty() && !before().isEmpty() && !history_.state().viewing();
    // Sections in the order they come first, objects in the order of their ids.
    std::map<std::string, QTreeWidgetItem*> sections;
    std::map<std::string, std::set<std::uint64_t>> objects_in;
    std::map<std::pair<std::string, std::uint64_t>, QTreeWidgetItem*> objects;
    for (const storage::DiffRow& row : rows_) {
        const std::string section_key = row.owner + '/' + row.type;
        QTreeWidgetItem*& section = sections[section_key];
        if (section == nullptr) {
            section = new QTreeWidgetItem(differences_, {to_qstring(row.section)});
            QFont font = section->font(0);
            font.setBold(true);
            section->setFont(0, font);
            section->setFirstColumnSpanned(true);
            section->setExpanded(true);
        }
        if (!row.id.is_valid()) {
            // The units: a field of the model itself.
            auto* item = new QTreeWidgetItem(
                section, {to_qstring(row.field), to_qstring(row.before), to_qstring(row.after)});
            tint(item, 1, before_tint);
            tint(item, 2, after_tint);
            continue;
        }
        objects_in[section_key].insert(row.id.value());
        QTreeWidgetItem*& object = objects[{section_key, row.id.value()}];
        const bool whole = row.kind != storage::ChangeKind::changed;
        if (object == nullptr) {
            object = new QTreeWidgetItem(section, {to_qstring(row.object)});
            object->setData(0, object_role, QVariant::fromValue<qulonglong>(row.id.value()));
            object->setExpanded(true);
            if (checkable) {
                object->setFlags(object->flags() | Qt::ItemIsUserCheckable);
                object->setCheckState(0, Qt::Unchecked);
            }
            if (whole) {
                const bool added = row.kind == storage::ChangeKind::added;
                object->setText(added ? 2 : 1, to_qstring(row.field));
                object->setText(added ? 1 : 2, text("comparison.absent"));
                for (int column = 0; column < 3; ++column) {
                    tint(object, column, added ? after_tint : before_tint);
                }
            }
        }
        if (whole && row.before.empty() && row.after.empty()) {
            continue;  // Added or removed: said on the row of the object.
        }
        auto* item = new QTreeWidgetItem(
            object, {to_qstring(row.field), to_qstring(row.before), to_qstring(row.after)});
        if (row.raw) {
            item->setToolTip(0, text("comparison.raw", {{"module", row.owner}}));
        }
        tint(item, 1, before_tint);
        tint(item, 2, after_tint);
    }
    for (const auto& [key, section] : sections) {
        const auto count = static_cast<std::int64_t>(objects_in[key].size());
        if (count > 0) {
            section->setText(0,
                             text("comparison.section",
                                  {{"section", section->text(0).toStdString()}, {"count", count}}));
        }
    }
    const bool same = rows_.empty();
    differences_->setVisible(!same);
    same_->setVisible(same);
    same_->setText(before().isEmpty() ? text("comparison.pick") : text("comparison.same"));
    rebuilding_ = false;
}

void ComparisonView::fill_indicators(const storage::ModelDiff& diff) {
    Workbench& workbench = history_.workbench();
    const storage::ResultIndicators a = indicators_of(history_, result_version(before()));
    const storage::ResultIndicators b = indicators_of(history_, result_version(after()));
    // Indicators of the modules in their order, then the others by id.
    std::vector<std::string> ids;
    std::map<std::string, const extension::ResultIndicatorContribution*> known;
    for (const extension::ResultIndicatorContribution* indicator :
         workbench.extensions.registry().result_indicators()) {
        known.emplace(indicator->id, indicator);
        if (a.contains(indicator->id) || b.contains(indicator->id)) {
            ids.push_back(indicator->id);
        }
    }
    std::set<std::string> others;
    for (const storage::ResultIndicators* values : {&a, &b}) {
        for (const auto& [id, value] : *values) {
            if (!known.contains(id)) {
                others.insert(id);
            }
        }
    }
    ids.insert(ids.end(), others.begin(), others.end());

    indicators_->clearContents();
    indicators_->setRowCount(static_cast<int>(ids.size()));
    indicators_->setHorizontalHeaderLabels({text("comparison.indicator"), state_title(before()),
                                            state_title(after()), text("comparison.difference")});
    const l10n::Localization& localization = workbench.localization;
    for (std::size_t row = 0; row < ids.size(); ++row) {
        const std::string& id = ids[row];
        const auto contribution = known.find(id);
        const model::Quantity* quantity =
            contribution != known.end() && !contribution->second->quantity.empty()
                ? model::find_quantity(contribution->second->quantity)
                : nullptr;
        const QString title = contribution != known.end()
                                  ? to_qstring(localization.manifest_text(
                                        contribution->second->owner, contribution->second->title))
                                  : to_qstring(id);
        const auto shown = [&](const storage::ResultIndicators& values,
                               const model::UnitSystem& units) -> QString {
            const auto found = values.find(id);
            if (found == values.end()) {
                return text("comparison.absent");
            }
            std::string value = l10n::to_text(found->second);
            if (quantity != nullptr) {
                const model::DisplayUnit* unit = model::model_unit(*quantity, units);
                value += ' ';
                value += unit != nullptr
                             ? localization.text(model::core_owner, "unit." + std::string(unit->id))
                             : model::compose_symbol(quantity->dimension, units);
            }
            return to_qstring(value);
        };
        QString difference = text("comparison.absent");
        const auto in_a = a.find(id);
        const auto in_b = b.find(id);
        if (in_a != a.end() && in_b != b.end()) {
            // Compared in SI: the two states may have other units.
            const double before_si =
                quantity ? model::to_si(*quantity, diff.units_before, in_a->second) : in_a->second;
            const double after_si =
                quantity ? model::to_si(*quantity, diff.units_after, in_b->second) : in_b->second;
            if (before_si != 0.0) {
                const double percent =
                    std::round((after_si - before_si) / std::abs(before_si) * 1000.0) / 10.0;
                difference = text("comparison.percent",
                                  {{"value", (percent > 0 ? "+" : "") + l10n::to_text(percent)}});
            } else if (after_si == 0.0) {
                difference = text("comparison.percent", {{"value", std::string("0")}});
            }
        }
        const int r = static_cast<int>(row);
        indicators_->setItem(r, 0, new QTableWidgetItem(title));
        indicators_->setItem(r, 1, new QTableWidgetItem(shown(a, diff.units_before)));
        indicators_->setItem(r, 2, new QTableWidgetItem(shown(b, diff.units_after)));
        indicators_->setItem(r, 3, new QTableWidgetItem(difference));
    }
    const bool none = ids.empty();
    indicators_->setVisible(!none);
    no_indicators_->setVisible(none);
    no_indicators_->setText(text("comparison.noIndicators"));
}

std::optional<std::string> ComparisonView::result_version(const QString& state) const {
    const HistoryState& history = history_.state();
    if (state.isEmpty()) {
        if (!history.version || history_.has_changes()) {
            return std::nullopt;  // The model now was not computed as a version.
        }
        return history.version;
    }
    const std::string name = state.toStdString();
    if (const auto variant = history.branches.find(name); variant != history.branches.end()) {
        return variant->second;
    }
    if (const auto label = history.tags.find(name); label != history.tags.end()) {
        return label->second;
    }
    return name;
}

QString ComparisonView::state_title(const QString& state) const {
    if (state.isEmpty()) {
        return text("comparison.currentModel");
    }
    const HistoryState& history = history_.state();
    const std::string name = state.toStdString();
    if (history.branches.contains(name)) {
        return text("comparison.variant",
                    {{"variant", history_.variant_title(name).toStdString()}});
    }
    if (history.tags.contains(name)) {
        return text("comparison.label", {{"label", name}});
    }
    if (const storage::VersionInfo* info = history.find(name)) {
        return text("comparison.version", {{"time", history_.time(info->time).toStdString()},
                                           {"message", info->summary}});
    }
    return state;
}

std::vector<model::ObjectId> ComparisonView::checked() const {
    std::vector<model::ObjectId> ids;
    for (int s = 0; s < differences_->topLevelItemCount(); ++s) {
        const QTreeWidgetItem* section = differences_->topLevelItem(s);
        for (int o = 0; o < section->childCount(); ++o) {
            const QTreeWidgetItem* object = section->child(o);
            if ((object->flags() & Qt::ItemIsUserCheckable) != 0 &&
                object->checkState(0) == Qt::Checked) {
                ids.emplace_back(object->data(0, object_role).toULongLong());
            }
        }
    }
    return ids;
}

bool ComparisonView::set_checked(model::ObjectId id, bool checked) {
    for (int s = 0; s < differences_->topLevelItemCount(); ++s) {
        QTreeWidgetItem* section = differences_->topLevelItem(s);
        for (int o = 0; o < section->childCount(); ++o) {
            QTreeWidgetItem* object = section->child(o);
            if ((object->flags() & Qt::ItemIsUserCheckable) != 0 &&
                object->data(0, object_role).toULongLong() == id.value()) {
                object->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
                return true;
            }
        }
    }
    return false;
}

bool ComparisonView::restore_checked() {
    const std::vector<model::ObjectId> ids = checked();
    if (ids.empty() || before().isEmpty() || !after().isEmpty()) {
        return false;
    }
    const std::optional<std::string> version = result_version(before());
    if (!version || !history_.restore_objects(*version, ids)) {
        return false;
    }
    refresh();
    return true;
}

void ComparisonView::update_restore() {
    const bool possible = after().isEmpty() && !before().isEmpty();
    restore_->setVisible(possible);
    restore_->setEnabled(possible && !history_.state().viewing() && !checked().empty());
    restore_->setText(text("comparison.restore", {{"state", state_title(before()).toStdString()}}));
}

void ComparisonView::retranslate() {
    title_->setText(text("comparison.title"));
    close_->setText(text("comparison.close"));
    swap_->setToolTip(text("comparison.swap"));
    indicators_title_->setText(text("comparison.indicators"));
    rebuild_selectors();
    refresh();
}

QString ComparisonView::text(std::string_view key, const l10n::Args& args) const {
    return history_.text(key, args);
}

}  // namespace fem::shell
