#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/model/object_id.h"
#include "fem/core/signal.h"
#include "fem/storage/diff.h"
#include "fem/storage/repository.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fem::storage {

class ProjectSession;

/// The history of a project (`fem.history`, docs/architecture.md §3.1, docs/interface-design.md
/// §12, docs/adr/0019-history.md): versions, variants, labels and changes put aside, kept in the
/// git repository of the project folder (`Repository`). Every `ProjectSession` has one.
///
/// A version is made from the document, not from the folder: committing saves the project too, so
/// the folder then holds that version. Ctrl/Cmd+S and autosave only save; what differs from the
/// version is "changes not in the history" (`has_changes`). Each version keeps the journal of what
/// was done since the version before it (`journal`); the journal of the changes since the last
/// version lives in `.fem/journal.slpy` of the folder between sessions.
///
/// Actions are written to the journal as `stresslab.history.commit("...")` and so on, with the
/// names and versions as they were given; restoring one object is the command
/// "history.restore_object" of the bus, one step of undo; restoring a part of the model (only the
/// materials, only the loads, a few objects) is "history.restore_objects". Actions are refused
/// inside a command or a transaction (CommandError(invalid_state)) and need a project folder
/// (ProjectError(no_folder)). Merging is not done (docs/plan.md, section 5).
class ProjectHistory {
public:
    /// The id of the command that restores one object.
    static constexpr std::string_view restore_object_command = "history.restore_object";
    /// The id of the command that restores a part of the model (task 2.4).
    static constexpr std::string_view restore_objects_command = "history.restore_objects";
    /// The owner of those commands.
    static constexpr std::string_view owner = "fem.history";

    ProjectHistory(const ProjectHistory&) = delete;
    ProjectHistory& operator=(const ProjectHistory&) = delete;
    ProjectHistory(ProjectHistory&&) = delete;
    ProjectHistory& operator=(ProjectHistory&&) = delete;
    ~ProjectHistory();

    // --- What there is ---------------------------------------------------------------------------
    // Without a folder or a history these give nothing: no versions, variant "main".

    /// The variant the next version goes to; none while a version is viewed on its own.
    [[nodiscard]] std::optional<std::string> current_branch() const;
    /// The version the document was opened from or last committed as; none before the first one.
    [[nodiscard]] std::optional<std::string> current_version() const;
    /// The model differs from the current version (or, without versions, from an empty model).
    /// Saves the bulk data of the document to the project folder to compare it.
    [[nodiscard]] bool has_changes() const;
    /// Versions newest first: of `target` (a variant, a label or a version), of the current
    /// version if none, or of all variants and labels if `all`.
    [[nodiscard]] std::vector<VersionInfo> log(std::optional<std::string_view> target = {},
                                               bool all = false, std::size_t limit = 0) const;
    [[nodiscard]] VersionInfo info(std::string_view target) const;
    /// The journal of a version: what was done since the version before it, as a script.
    [[nodiscard]] std::string journal(std::string_view target) const;
    /// The id of a version (see `Repository::resolve`). Throws ProjectError(unknown_version).
    [[nodiscard]] std::string resolve(std::string_view target) const;
    [[nodiscard]] std::map<std::string, std::string> branches() const;
    [[nodiscard]] std::map<std::string, std::string> tags() const;
    /// Changes put aside, newest first.
    [[nodiscard]] std::vector<VersionInfo> stashes() const;
    /// How the model of `to` differs from that of `from` (task 2.4), versions named as `resolve`
    /// names them; without `to`, the model now, with its changes not in the history yet (its bulk
    /// data is saved to the project folder to compare it, as `has_changes` does). Not written to
    /// the journal. Throws ProjectError(unknown_version).
    [[nodiscard]] ModelDiff diff(std::string_view from,
                                 std::optional<std::string_view> to = {}) const;
    /// The blocks of bulk data every version needs: roots for "Clean project".
    [[nodiscard]] std::vector<std::string> blob_roots() const;

    // --- Actions ---------------------------------------------------------------------------------

    /// "Commit version": saves the project and makes a version of it on the current variant;
    /// returns its id. Throws ProjectError: invalid_name for an empty message, detached while a
    /// version is viewed on its own, nothing_to_commit if the model is that of the current version.
    std::string commit(std::string_view message);
    /// An automatic version, as before something risky: made if the model has changes or there
    /// are no versions yet; returns the version the model now is. Throws ProjectError(detached)
    /// for changes while a version is viewed on its own.
    std::string snapshot(std::string_view reason);
    /// A new variant at `at` (the current version if none); stays on the current variant.
    /// Throws ProjectError: invalid_name, already_exists, unknown_version.
    void branch(std::string_view name, std::optional<std::string_view> at = {});
    /// Opens a variant to work on, or a label or a version to look at (it is viewed on its own:
    /// make a variant from it to change it). Saves the project. Throws ProjectError:
    /// uncommitted_changes if the model has changes, unknown_version.
    void checkout(std::string_view target);
    /// A label at `at` (the current version if none). Throws as `branch` does.
    void tag(std::string_view name, std::optional<std::string_view> at = {});
    /// Puts the changes aside and brings back the current version. Throws ProjectError:
    /// nothing_to_stash without changes, unknown_version before the first version.
    void stash(std::string_view message = {});
    /// Brings back the changes put aside at `index` and forgets them. Throws ProjectError:
    /// unknown_version, uncommitted_changes, stash_conflict if the current version is not the one
    /// they were put aside on (bringing them over would be a merge).
    void stash_pop(std::size_t index = 0);
    /// Forgets the changes put aside at `index`. Throws ProjectError(unknown_version).
    void stash_drop(std::size_t index = 0);
    /// "Restore this version": a new version on the current variant with the model of `target`;
    /// nothing is lost: changes not in the history become an automatic version first, and the
    /// versions after `target` stay. Ids are not given out again: the next id is the greater of
    /// the two. Returns the id of the new version, or of the current one if the model is the same.
    /// Throws ProjectError: detached, unknown_version.
    std::string restore_version(std::string_view target);

    /// Who signs new versions; by default the name of the user of the system.
    void set_author(Author author);
    [[nodiscard]] const Author& author() const noexcept { return author_; }

    /// Called after an action changed the history or where the project is in it (versions,
    /// variants, labels, changes put aside, HEAD), and after the results of the project changed
    /// (`ResultCache`): the panel "History" of the window follows it (task 2.6). Not called for
    /// changes of the model alone, nor when another project is opened (`ProjectSession::on_changed`
    /// tells that).
    [[nodiscard]] Subscription on_changed(std::function<void()> listener);

private:
    friend class ProjectSession;
    friend class ResultCache;

    ProjectHistory(ProjectSession& session, command::CommandBus& bus);

    // For ProjectSession.
    void opened(const std::filesystem::path& folder);
    void closed();
    void saved(const std::filesystem::path& folder);

    [[nodiscard]] const std::filesystem::path& folder() const;
    [[nodiscard]] std::optional<Repository> repository() const;
    [[nodiscard]] Repository repository_for_writing() const;
    [[nodiscard]] ProjectFiles current_files() const;
    /// The files differ from the current version of `repo`, or from an empty model without one.
    [[nodiscard]] bool differs(const Repository* repo, const ProjectFiles& files) const;
    /// The journal of the changes since the last version, as a script.
    [[nodiscard]] std::string pending_journal() const;
    /// Makes a version of `files` on the current variant (or as a stash) and moves the variant.
    std::string make_version(Repository& repo, const ProjectFiles& files, std::string journal,
                             const VersionNote& note);
    /// Snapshot for a command with `snapshot_before`; not journaled: the command implies it.
    void snapshot_before(const command::CommandSpec& spec);
    std::string snapshot(std::string_view reason, std::string_view before, bool journaled);
    /// Makes the document and the folder hold `version`, journaling `entry`; with `next_id`, the
    /// next object id is at least that. Returns the files the folder now has.
    ProjectFiles load(const VersionFiles& version, std::string_view entry,
                      std::optional<std::uint64_t> next_id = {});
    void forget_pending();
    command::CommandResult restore_object(command::CommandContext& context);
    command::CommandResult restore_objects(command::CommandContext& context);
    void check_idle(std::string_view action) const;

    ProjectSession& session_;
    command::CommandBus& bus_;
    Author author_;
    /// Journal entries since the last version.
    std::vector<std::string> pending_;
    Signal<> changed_;
    Subscription journal_;
    Subscription before_command_;
};

/// The blocks of bulk data the history `repo` needs: those of every version a variant, a label,
/// HEAD or a stash reaches, and those of the pinned results (task 2.5). The roots of "Clean
/// project" and what a package with the history carries.
[[nodiscard]] std::vector<std::string> history_blob_roots(const Repository& repo);

}  // namespace fem::storage
