#pragma once

#include "fem/core/signal.h"
#include "fem/shell/history_graph.h"
#include "fem/shell/workbench.h"

#include <QString>
#include <QWidget>
#include <map>
#include <optional>
#include <string>
#include <vector>

class QAction;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;
class QToolButton;

namespace fem::shell {

class HistoryController;
struct HistoryRowView;

/// The panel "History" (task 2.6, docs/interface-design.md §12): the variant to work on, "Commit
/// version" with the changes listed by the application, the graph of versions and variants, and
/// the changes put aside. A tab next to the model tree, opened by the activity "History".
///
/// A row of the graph is a version: its lane and node, chips of the variants and labels at it, its
/// message (grey for an automatic version), and below it the author, the time, the sections of
/// the model tree it changed and whether it has results. Clicking a version opens it to look at
/// (`HistoryController::view`); its context menu has "New variant from here", "Restore this
/// version", "Add label", "Compare with the current model" and "Compare with marked".
///
/// Object names, for tests: "history.variants", "history.commit", "history.compare",
/// "history.filters" (with the actions "history.filter.automatic", "history.filter.results",
/// "history.filter.labels" and the author menu "history.filter.author"), "history.commitPane"
/// with "history.message", "history.changes", "history.commitOk", "history.commitCancel";
/// "history.graph", "history.empty", "history.stashes" with "history.stashPop",
/// "history.stashDrop"; the action "history.commitVersion" (Ctrl/Cmd+Shift+S).
class HistoryPanel : public QWidget {
    Q_OBJECT

public:
    /// Item data of the rows of the graph.
    enum Role {
        version_role = Qt::UserRole,  ///< The id of the version.
        row_role,                     ///< The index of its row in `layout()`.
    };

    HistoryPanel(HistoryController& history, QWidget* parent = nullptr);
    ~HistoryPanel() override;
    HistoryPanel(const HistoryPanel&) = delete;
    HistoryPanel& operator=(const HistoryPanel&) = delete;
    HistoryPanel(HistoryPanel&&) = delete;
    HistoryPanel& operator=(HistoryPanel&&) = delete;

    /// "Commit version" (Ctrl/Cmd+Shift+S): opens the commit pane; for the "File" menu and the
    /// palette.
    [[nodiscard]] QAction* commit_action() const noexcept { return commit_action_; }
    /// Opens the commit pane with the changes of the model listed; false if there is no variant
    /// to commit to (no folder, a version is viewed).
    bool start_commit();
    /// Commits with the text of the pane (the automatic message if it is empty) and closes it.
    bool finish_commit();
    void cancel_commit();
    [[nodiscard]] bool is_committing() const;

    /// The graph as it is shown.
    [[nodiscard]] const GraphLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] QListWidget& graph() const noexcept { return *graph_; }
    /// The version of a row; empty if there is no such row.
    [[nodiscard]] std::string version_at(int row) const;
    /// The row of a version; -1 if it is not shown.
    [[nodiscard]] int row_of(const std::string& version) const;

    // Filters: automatic versions (shown by default), only versions with results, only versions
    // with labels, versions of one author (empty: everyone's).
    void set_show_automatic(bool show);
    void set_only_with_results(bool only);
    void set_only_with_labels(bool only);
    void set_author(const QString& name);

    /// The sections of the model tree a version changed since the version it was made on, as
    /// titles ("Material, Mesh"); read once per version.
    [[nodiscard]] QString changed_sections(const std::string& version);

    /// The context menu of a version; the caller owns it.
    [[nodiscard]] QMenu* version_menu(const std::string& version, QWidget* parent);
    /// "Mark for comparison": the version the next "Compare with marked" compares with.
    void mark_for_comparison(const std::string& version);
    [[nodiscard]] const std::string& marked() const noexcept { return marked_; }
    /// "Compare": two selected versions, the older first; one with the current model; none, the
    /// current version with the current model.
    void compare_selected();

    void retranslate();

Q_SIGNALS:
    /// Compare two states: versions by id, an empty one is the current model.
    void compare_requested(const QString& before, const QString& after);

private:
    friend class HistoryGraphDelegate;

    void rebuild();
    void rebuild_variants();
    void rebuild_stashes();
    void rebuild_author_menu();
    void show_menu(const QPoint& position);
    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;

    HistoryController& history_;
    Workbench& workbench_;

    QComboBox* variants_ = nullptr;
    QToolButton* commit_button_ = nullptr;
    QToolButton* compare_button_ = nullptr;
    QToolButton* filter_button_ = nullptr;
    QMenu* filter_menu_ = nullptr;
    QMenu* author_menu_ = nullptr;
    QAction* show_automatic_ = nullptr;
    QAction* only_results_ = nullptr;
    QAction* only_labels_ = nullptr;
    QAction* commit_action_ = nullptr;

    QFrame* commit_pane_ = nullptr;
    QLabel* commit_title_ = nullptr;
    QLineEdit* message_ = nullptr;
    QLabel* changes_title_ = nullptr;
    QListWidget* changes_ = nullptr;
    QPushButton* commit_ok_ = nullptr;
    QPushButton* commit_cancel_ = nullptr;

    QListWidget* graph_ = nullptr;
    QLabel* empty_ = nullptr;

    QFrame* stashes_ = nullptr;
    QLabel* stashes_title_ = nullptr;
    QListWidget* stash_list_ = nullptr;
    QPushButton* stash_pop_ = nullptr;
    QPushButton* stash_drop_ = nullptr;

    GraphLayout layout_;
    /// The versions `layout_` was made of, in its order.
    std::vector<std::string> laid_out_;
    /// What each row of `layout_` shows.
    std::vector<HistoryRowView> views_;
    QString author_;
    std::string marked_;
    /// Sections changed by each version, as read.
    std::map<std::string, QString> sections_;
    bool rebuilding_ = false;
    Subscription language_;
};

}  // namespace fem::shell
