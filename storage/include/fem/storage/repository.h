#pragma once

#include "fem/storage/project_format.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The history of a project folder (docs/architecture.md §3.1, docs/adr/0019-history.md): a bare
// git repository in `.fem/git`, kept with libgit2. A version (commit) holds the files of the
// project as `ProjectFiles` and, as `.fem/journal.slpy`, the journal of what was done since the
// version before it. Bulk data is not in git: `refs/blobs.json` refers to the blocks of
// `.fem/objects`. There is no working tree, index or checkout of libgit2: trees are made from the
// files in memory and read back into them, so git holds the same bytes as the folder.
//
// Variants are branches and labels are tags, with any names users give them: in references the
// names are escaped (`ref_segment`), and they are given back as they were.
namespace fem::storage {

/// The folder of the history inside a project folder.
inline constexpr std::string_view history_folder = ".fem/git";
/// The journal of a version inside its tree, and the journal since the last version in a folder.
inline constexpr std::string_view version_journal_file = ".fem/journal.slpy";
/// The variant of a new history.
inline constexpr std::string_view main_branch = "main";

/// Who made a version. Neither part may be empty or hold '<', '>' or line breaks.
struct Author {
    std::string name;
    std::string email;
};

/// Why a version exists.
enum class VersionKind : std::uint8_t {
    manual,     ///< "Commit version": a checkpoint the user made.
    automatic,  ///< Made by the application before something risky or before a run.
    restore,    ///< "Restore this version": the state of an older version, made anew.
    stash,      ///< Changes put aside; not on any variant.
};

/// "manual", "automatic", "restore", "stash".
[[nodiscard]] std::string_view to_string(VersionKind kind) noexcept;

/// What a new version says about itself.
struct VersionNote {
    std::string message{};  ///< The text the user sees; not empty.
    VersionKind kind = VersionKind::manual;
    std::string before{};    ///< automatic: the id of the command it was made before, if any.
    std::string restored{};  ///< restore: the id of the version restored.
    std::string branch{};    ///< stash: the variant the changes were put aside on.
};

/// A version as the history shows it.
struct VersionInfo {
    std::string id;                    ///< 40 hex digits.
    std::vector<std::string> parents;  ///< Ids; the first is the version it was made on.
    std::string message;               ///< As given, without the trailers the history adds.
    std::string summary;               ///< The first line of the message.
    Author author;
    std::int64_t time = 0;  ///< Seconds since 1970-01-01 UTC.
    int time_offset = 0;    ///< Minutes east of UTC where it was made.
    VersionKind kind = VersionKind::manual;
    std::string before;
    std::string restored;
    std::string branch;
};

/// The files of a version and its journal.
struct VersionFiles {
    ProjectFiles files;   ///< project.json, model/..., refs/blobs.json.
    std::string journal;  ///< The text of `.fem/journal.slpy`; empty if the version has none.
};

/// Where HEAD is.
struct Head {
    /// The variant HEAD is on; none while a version is viewed on its own (detached).
    std::optional<std::string> branch;
    /// The version HEAD points at; none before the first version of its variant.
    std::optional<std::string> version;
};

/// The git repository of a project folder. Moving it is cheap; one thread at a time.
///
/// Errors are ProjectError: `io` for what libgit2 refuses (the text of libgit2 in the message),
/// `unknown_version` for a version, variant or label that is not there, `already_exists`,
/// `invalid_name` for a name that cannot be a variant or a label (empty, not UTF-8, line breaks).
class Repository {
public:
    /// The history of `project_folder`, or none if it has none yet.
    [[nodiscard]] static std::optional<Repository> open(
        const std::filesystem::path& project_folder);
    /// The history of `project_folder`, made empty on the variant "main" if it has none.
    [[nodiscard]] static Repository open_or_create(const std::filesystem::path& project_folder);

    Repository(Repository&& other) noexcept;
    Repository& operator=(Repository&& other) noexcept;
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;
    ~Repository();

    // --- Versions --------------------------------------------------------------------------------

    /// Writes a version of `files` with `journal` and returns its id. Moves no variant or label.
    std::string commit(const ProjectFiles& files, std::string_view journal, const VersionNote& note,
                       const Author& author, std::span<const std::string> parents);
    /// The files of a version (`id` as `resolve` returns it).
    [[nodiscard]] VersionFiles read(std::string_view id) const;
    /// One file of a version by its path ("refs/blobs.json"); none if the version has no such
    /// file.
    [[nodiscard]] std::optional<std::string> read_file(std::string_view id,
                                                       std::string_view path) const;
    [[nodiscard]] VersionInfo info(std::string_view id) const;
    /// Versions reachable from `from`, newest first, parents after their children; `limit` 0 is
    /// all of them.
    [[nodiscard]] std::vector<VersionInfo> log(std::span<const std::string> from,
                                               std::size_t limit = 0) const;
    /// Every version a variant, a label, HEAD or a stash reaches, each once.
    [[nodiscard]] std::vector<std::string> reachable() const;

    /// The id of a version: a variant ("main", "Композитная обшивка"), a label, an id or the start
    /// of one (at least 4 hex digits), each optionally with "~N", N versions back along the first
    /// parents. An existing variant or label wins over an id it looks like. Throws
    /// ProjectError(unknown_version).
    [[nodiscard]] std::string resolve(std::string_view target) const;

    // --- HEAD, variants and labels ---------------------------------------------------------------

    [[nodiscard]] Head head() const;
    /// Puts HEAD on a variant, which may have no versions yet.
    void attach_head(std::string_view branch);
    /// Puts HEAD on a version alone.
    void detach_head(std::string_view id);

    /// Variants with their last versions, by name; a variant without versions is not listed.
    [[nodiscard]] std::map<std::string, std::string> branches() const;
    [[nodiscard]] std::optional<std::string> branch(std::string_view name) const;
    /// Makes a variant point at `id`. Throws ProjectError(already_exists) if `create_only` and the
    /// variant exists (on a file system that ignores case, also one that differs in case only).
    void set_branch(std::string_view name, std::string_view id, bool create_only);

    [[nodiscard]] std::map<std::string, std::string> tags() const;
    /// Throws ProjectError(already_exists) if the label exists.
    void create_tag(std::string_view name, std::string_view id);

    // --- Stash -----------------------------------------------------------------------------------

    /// Changes put aside, newest first: ids of versions of kind stash.
    [[nodiscard]] std::vector<std::string> stashes() const;
    /// Puts a version of kind stash in front of the others.
    void push_stash(std::string_view id);
    /// Forgets the stash at `index` of `stashes()`. Throws ProjectError(unknown_version).
    void drop_stash(std::size_t index);

private:
    struct State;
    explicit Repository(std::unique_ptr<State> state);

    std::unique_ptr<State> state_;
};

/// True if `name` can name a variant or a label: UTF-8 of 1 to 64 characters without control
/// characters, not too long as a file name.
[[nodiscard]] bool is_valid_version_name(std::string_view name) noexcept;

}  // namespace fem::storage
