#pragma once

#include "fem/core/model/object_id.h"
#include "fem/core/signal.h"
#include "fem/shell/workbench.h"
#include "fem/storage/repository.h"
#include "fem/storage/result_cache.h"

#include <QObject>
#include <QString>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QWidget;

namespace fem::shell {

/// What the window shows of the history of the project (task 2.6): read again whenever the history,
/// the project folder or the setting of the terms changes.
struct HistoryState {
    /// The project has a folder: there can be a history.
    bool has_folder = false;
    /// The variant the next version goes to; none while a version is viewed.
    std::optional<std::string> branch;
    /// The version the model is (or was last committed as); none before the first one.
    std::optional<std::string> version;
    /// Versions of every variant, label and HEAD, newest first, children before their parents.
    std::vector<storage::VersionInfo> versions;
    /// Variants and labels with their last versions.
    std::map<std::string, std::string> branches;
    std::map<std::string, std::string> tags;
    /// Changes put aside, newest first.
    std::vector<storage::VersionInfo> stashes;
    /// Results of the cache and pinned ones, used last first.
    std::vector<storage::ResultInfo> results;

    /// A version is viewed on its own: the model is read-only.
    [[nodiscard]] bool viewing() const noexcept { return has_folder && !branch && version; }
    /// The version of this id, or nullptr.
    [[nodiscard]] const storage::VersionInfo* find(std::string_view id) const;
};

/// The history of the project in the window (task 2.6, docs/interface-design.md §12,
/// docs/adr/0022-history-ui.md): what the panel "History", the bar over the 3D view, the switcher
/// of variants and the comparison show and do.
///
/// - Viewing a version opens it on its own (`ProjectHistory::checkout` of its id) and makes the
///   document read-only (`CommandBus::set_read_only`), the Python console of the window included;
///   "Back to current" opens the variant it came from.
/// - Before another variant or version opens, changes not in the history are committed or put
///   aside, as the user chooses (`Workbench::ask_pending`).
/// - Failures are not modal: their message goes out through `problem`, what was done through
///   `message`. Every action is a call of `ProjectHistory` and is journaled by it.
///
/// Without `Workbench::project` there is no history: the state is empty and actions do nothing.
class HistoryController : public QObject {
    Q_OBJECT

public:
    /// `window` is the parent of dialogs.
    HistoryController(Workbench& workbench, QWidget* window);
    ~HistoryController() override;
    HistoryController(const HistoryController&) = delete;
    HistoryController& operator=(const HistoryController&) = delete;
    HistoryController(HistoryController&&) = delete;
    HistoryController& operator=(HistoryController&&) = delete;

    [[nodiscard]] const HistoryState& state() const noexcept { return state_; }
    [[nodiscard]] Workbench& workbench() const noexcept { return workbench_; }
    /// Reads the state again and tells `changed`.
    void refresh();

    // --- Words -----------------------------------------------------------------------------------

    /// A string of the shell in the terms the user chose (`history_text`).
    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;
    /// The name of a variant as shown (`variant_title`).
    [[nodiscard]] QString variant_title(std::string_view name) const;
    /// What the switcher and the title of the window say: the variant, or "Version of <time>"
    /// while one is viewed; empty without a folder.
    [[nodiscard]] QString current_title() const;
    /// The local time of a version.
    [[nodiscard]] QString time(std::int64_t seconds) const;

    // --- Changes not in the history --------------------------------------------------------------

    /// The changes of the model since its version, a line each (`storage::describe`); empty if
    /// there is no folder, no version or no change.
    [[nodiscard]] std::vector<std::string> pending_changes() const;
    /// The message a version gets if the user leaves it to the application: the first changes
    /// joined by "; ", or "First version".
    [[nodiscard]] QString automatic_message() const;
    /// The model differs from its version (`ProjectHistory::has_changes`).
    [[nodiscard]] bool has_changes() const;

    // --- Actions ---------------------------------------------------------------------------------
    // Each returns false if it did not happen (cancelled or failed).

    /// "Commit version": `message`, or the automatic one if it is empty.
    bool commit(const QString& message);
    /// Opens a variant to work on.
    bool switch_to(const std::string& variant);
    /// Opens a version only to look at it.
    bool view(const std::string& version);
    /// "Back to current": opens the variant the version was viewed from (or the main variant, or
    /// the variant whose line has the version).
    bool return_to_current();
    /// "New variant from here": a variant at `version` (the current one if empty), opened at
    /// once. Asks for its name unless one is given.
    bool make_variant(const std::string& version, const QString& name = {});
    /// "Restore this version": a new version on the current variant (the one it was viewed from,
    /// opened first) with the model of `version`.
    bool restore(const std::string& version);
    /// "Restore the checked objects": the objects `ids` as they are in `version`, one step of undo.
    bool restore_objects(const std::string& version, const std::vector<model::ObjectId>& ids);
    /// A label at `version`. Asks for its name unless one is given.
    bool tag(const std::string& version, const QString& name = {});
    /// Brings back or forgets changes put aside.
    bool stash_pop(std::size_t index);
    bool stash_drop(std::size_t index);

    // --- Results ---------------------------------------------------------------------------------

    /// The results computed from `version`, used last first.
    [[nodiscard]] std::vector<const storage::ResultInfo*> results_of(
        std::string_view version) const;
    /// The variant has results, but not of its last version: they are out of date (⚠ in the
    /// graph).
    [[nodiscard]] bool results_outdated(std::string_view variant) const;

Q_SIGNALS:
    /// The state was read again.
    void changed();
    /// The setting "history.gitTerms" changed: texts are to be set again.
    void terms_changed();
    /// A message for the user about a failure.
    void problem(const QString& message);
    /// A message for the user about what was done.
    void message(const QString& message);

private:
    /// Commits or puts aside the changes not in the history if there are any and the user wants
    /// to; false if the user cancelled or it failed.
    bool settle_changes();
    /// Opens `target` (a variant or a version) after settling the changes.
    bool open(const std::string& target, std::string_view action_key);
    [[nodiscard]] QString ask_name(std::string_view title_key, std::string_view label_key,
                                   const QString& text) const;
    void report(std::string_view action_key, const std::exception& error);
    void follow_settings(std::string_view key);
    /// The read-only state of the bus follows whether a version is viewed.
    void apply_read_only();

    Workbench& workbench_;
    QWidget* window_;
    HistoryState state_;
    /// The variant a viewed version was opened from.
    std::optional<std::string> came_from_;
    Subscription history_changed_;
    Subscription project_changed_;
    Subscription settings_changed_;
    Subscription language_;
};

}  // namespace fem::shell
