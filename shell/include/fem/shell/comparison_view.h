#pragma once

#include "fem/core/model/object_id.h"
#include "fem/core/signal.h"
#include "fem/storage/diff.h"

#include <QString>
#include <QWidget>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QToolButton;
class QTreeWidget;

namespace fem::shell {

class HistoryController;

/// The screen "Compare" (task 2.6, docs/interface-design.md §12, "Variants of a design", item 4),
/// in place of the 3D view: two states of the model, A (earlier) and B (later), each a version, a
/// variant, a label or the current model; the differences of the model by sections of the model
/// tree with "was / is" (`storage::describe_rows`); and the key indicators of the results of A and
/// B (`resultIndicators` of modules, `ResultInfo::indicators`) with the difference in percent.
/// Two synchronous 3D views come after the viewport of task 3.7.
///
/// When B is the current model, objects can be checked and restored as they are in A ("Restore
/// checked from A", `HistoryController::restore_objects`, one step of undo): restoring a part,
/// "only the materials, only the loads, one body" (§12, "Rollback").
///
/// Object names, for tests: "comparison", "comparison.before", "comparison.after",
/// "comparison.swap", "comparison.close", "comparison.differences", "comparison.same",
/// "comparison.restore", "comparison.indicators", "comparison.noIndicators".
class ComparisonView : public QWidget {
    Q_OBJECT

public:
    /// Item data of the rows of the differences: the id of the object of an object row.
    static constexpr int object_role = Qt::UserRole;

    explicit ComparisonView(HistoryController& history, QWidget* parent = nullptr);
    ~ComparisonView() override;
    ComparisonView(const ComparisonView&) = delete;
    ComparisonView& operator=(const ComparisonView&) = delete;
    ComparisonView(ComparisonView&&) = delete;
    ComparisonView& operator=(ComparisonView&&) = delete;

    /// Compares two states: versions (ids, variants or labels); an empty one is the current model.
    /// If A is the current model, the two are swapped: A is the earlier state.
    void compare(const QString& before, const QString& after);
    /// Compares what the selectors say now.
    void refresh();

    /// The ids of the states compared; empty for the current model.
    [[nodiscard]] QString before() const;
    [[nodiscard]] QString after() const;
    /// The rows of the differences shown.
    [[nodiscard]] const std::vector<storage::DiffRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] QTreeWidget& differences() const noexcept { return *differences_; }
    [[nodiscard]] QTableWidget& indicators() const noexcept { return *indicators_; }
    /// The objects checked to restore.
    [[nodiscard]] std::vector<model::ObjectId> checked() const;
    /// Checks or unchecks an object; false if it has no row that can be checked.
    bool set_checked(model::ObjectId id, bool checked);
    /// "Restore checked from A".
    bool restore_checked();

    void retranslate();

Q_SIGNALS:
    /// "Close": the window shows the 3D view again.
    void closed();

private:
    void rebuild_selectors();
    void fill_differences(const storage::ModelDiff& diff);
    void fill_indicators(const storage::ModelDiff& diff);
    void update_restore();
    /// The version whose results stand for a state: the version itself, or for the current model
    /// the current version if the model has no changes.
    [[nodiscard]] std::optional<std::string> result_version(const QString& state) const;
    [[nodiscard]] QString state_title(const QString& state) const;
    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;

    HistoryController& history_;
    QLabel* title_ = nullptr;
    QComboBox* before_ = nullptr;
    QComboBox* after_ = nullptr;
    QToolButton* swap_ = nullptr;
    QToolButton* close_ = nullptr;
    QTreeWidget* differences_ = nullptr;
    QLabel* same_ = nullptr;
    QPushButton* restore_ = nullptr;
    QLabel* indicators_title_ = nullptr;
    QTableWidget* indicators_ = nullptr;
    QLabel* no_indicators_ = nullptr;
    std::vector<storage::DiffRow> rows_;
    /// Versions asked for that are not variants or labels: they stay in the selectors.
    std::vector<std::string> picked_;
    bool rebuilding_ = false;
    Subscription language_;
    Subscription model_changed_;
};

}  // namespace fem::shell
