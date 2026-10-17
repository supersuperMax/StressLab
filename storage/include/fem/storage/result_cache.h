#pragma once

#include "fem/storage/progress.h"
#include "fem/storage/project_format.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The cache of results of calculations (docs/architecture.md §3.1, "Three kinds of data";
// docs/adr/0021-result-cache-and-slpack.md). Results are not in the history: they are kept in
// `.fem/cache/results/<key>/` of the project folder, under the key of what they were computed
// from (the model, its mesh, the solver and its version), each marked with the version of the
// model it was computed from. A result can be removed and computed again. A pinned result is kept
// in the history: its files go to the blob store and its record to the git repository, so it
// travels with the history and is never removed to make room.
//
// For the core a result is a set of files, whatever the solver wrote: reading them is the work
// of the modules (the Results IR, task 3.1).
namespace fem::storage {

class ProjectSession;
class Repository;

/// The folder of the cache inside a project folder.
inline constexpr std::string_view result_cache_folder = ".fem/cache/results";

/// The program a result was computed with.
struct SolverInfo {
    std::string id;       ///< As the module of the solver names it; not empty.
    std::string version;  ///< Of the program, not of the module; may be empty.

    friend bool operator==(const SolverInfo&, const SolverInfo&) = default;
};

/// What a run takes besides the model and changes its result (an option of the solver that is not
/// a part of the model): name -> value.
using ResultInputs = std::map<std::string, std::string, std::less<>>;

/// The key of the results of `solver` for the model of `files`: 64 hex digits of the SHA-256 of a
/// canonical text of the files of the model (every file under model/ by the hash of its bytes),
/// refs/blobs.json (the meshes by the hashes of their data), the units of project.json, the solver
/// and `inputs`. The versions of modules, the next object id and the journal are not a part of
/// it: they do not change a result. The same on every computer. Throws ProjectError
/// (invalid_file, unsupported_format) for files that are not a project of this format.
[[nodiscard]] std::string result_key(const ProjectFiles& files, const SolverInfo& solver,
                                     const ResultInputs& inputs = {});

/// A file of a result.
struct ResultFile {
    std::string path;  ///< Relative to the folder of the result, with '/'.
    std::uint64_t size = 0;
    std::string sha256;  ///< 64 hex digits of the bytes.

    friend bool operator==(const ResultFile&, const ResultFile&) = default;
};

/// A result as the cache shows it.
struct ResultInfo {
    std::string key;
    /// The version of the model it was computed from (40 hex digits).
    std::string version;
    SolverInfo solver;
    ResultInputs inputs;
    std::int64_t created = 0;       ///< When it was put in the cache: seconds since 1970-01-01 UTC.
    std::int64_t used = 0;          ///< When it was last put or found.
    std::vector<ResultFile> files;  ///< By path.
    std::uint64_t bytes = 0;        ///< The size of the files together.
    bool cached = false;            ///< Its files are in the cache.
    bool pinned = false;            ///< It is kept in the history.
    /// Where its files are, if they are in the cache.
    std::optional<std::filesystem::path> folder;
};

/// What "Remove the results of inactive variants" did.
struct PruneStats {
    std::size_t removed = 0;         ///< Results removed from the cache.
    std::uintmax_t freed_bytes = 0;  ///< The size of their files.
};

/// The results of the project of a `ProjectSession` (`session.results()`). The folder is that of
/// the session; without one there is no cache: `find` and `list` give nothing, the actions throw
/// ProjectError(no_folder). Like the session, it is used from one thread.
///
/// Pinning, unpinning, removing and pruning are written to the journal as
/// `stresslab.results.pin("<key>")` and so on and are refused inside a command or a transaction
/// (CommandError(invalid_state)); computing keys, finding, listing and putting are not written:
/// putting follows from the command that ran the solver.
class ResultCache {
public:
    /// Seconds since 1970-01-01 UTC; tests give their own.
    using Clock = std::function<std::int64_t()>;
    /// The default limit of the size of the cache, as the setting "results.cacheLimit" has it.
    static constexpr std::uint64_t default_limit = 20480ULL * 1024 * 1024;

    ResultCache(const ResultCache&) = delete;
    ResultCache& operator=(const ResultCache&) = delete;
    ResultCache(ResultCache&&) = delete;
    ResultCache& operator=(ResultCache&&) = delete;
    ~ResultCache();

    /// The key of the results of the model now (`result_key`), with its changes not in the
    /// history: its bulk data is saved to the project folder to see it, as
    /// `ProjectHistory::has_changes` does.
    [[nodiscard]] std::string key(const SolverInfo& solver, const ResultInputs& inputs = {}) const;
    /// The key of the results of the model of a version (named as `ProjectHistory::resolve`
    /// names it). Throws ProjectError(unknown_version).
    [[nodiscard]] std::string key_at(std::string_view version, const SolverInfo& solver,
                                     const ResultInputs& inputs = {}) const;

    /// Puts the files of `from` (a folder; all files below it, as they are) in the cache as the
    /// result of `key`, computed from `version` by `solver`, in place of one already there; then
    /// removes the results used longest ago while the cache is larger than its limit (never this
    /// one). The result is there whole or not at all. Throws ProjectError: no_folder,
    /// unknown_version, result_mismatch if `key` is not that of the model of `version` with this
    /// solver and these inputs, io.
    ResultInfo put(std::string_view key, std::string_view version, const SolverInfo& solver,
                   const ResultInputs& inputs, const std::filesystem::path& from);
    /// The result of `key`, which becomes the one used last. A pinned result whose files are not in
    /// the cache any more is brought back to it from the history. None if there is no such result.
    /// Throws ProjectError (io, missing_blob, corrupt_blob) if it cannot be read.
    [[nodiscard]] std::optional<ResultInfo> find(std::string_view key);
    /// Every result, in the cache or pinned, used last first.
    [[nodiscard]] std::vector<ResultInfo> list() const;

    /// Keeps the result of `key`, which must be in the cache, in the history. Nothing happens to a
    /// pinned one. Throws ProjectError: no_folder, unknown_result, unknown_version if its version
    /// is not in the history, cancelled (then it is not pinned); and CommandError(invalid_state)
    /// inside a command or a transaction.
    void pin(std::string_view key, const Progress& progress = {});
    /// The result of `key` is not kept in the history any more; its blocks go with the next "Clean
    /// project", and its files stay in the cache if they are there. Throws ProjectError: no_folder,
    /// unknown_result if it is not pinned; and CommandError(invalid_state).
    void unpin(std::string_view key);
    /// Removes the files of the result of `key` from the cache; a pinned result stays in the
    /// history. False if they were not there. Throws ProjectError(no_folder, io) and
    /// CommandError(invalid_state).
    bool remove(std::string_view key);
    /// "Remove the results of inactive variants": removes from the cache every result whose
    /// version is not one of the current variant (the version HEAD is on and those before it).
    /// Pinned results stay in the history. Throws ProjectError(no_folder, io) and
    /// CommandError(invalid_state).
    PruneStats prune_inactive();

    /// The size the files in the cache may take; 0 is no limit. A smaller limit takes effect at
    /// the next `put`.
    void set_limit(std::uint64_t bytes) noexcept { limit_ = bytes; }
    [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }
    /// The size the files in the cache take now.
    [[nodiscard]] std::uint64_t usage() const;

    void set_clock(Clock clock);

private:
    friend class ProjectSession;

    explicit ResultCache(ProjectSession& session);

    [[nodiscard]] std::filesystem::path cache_folder() const;
    /// The results in the cache; none without a folder.
    [[nodiscard]] std::vector<ResultInfo> cached() const;
    void evict(std::string_view keep);
    void check_idle(std::string_view action) const;

    ProjectSession& session_;
    Clock clock_;
    std::uint64_t limit_ = default_limit;
};

/// The blocks of bulk data the pinned results of `repo` need: the tables of their files, and the
/// blocks of their versions (`refs/blobs.json`), so that "Clean project" keeps a pinned result
/// whole even if its version is reachable from nothing else.
[[nodiscard]] std::vector<std::string> pinned_result_roots(const Repository& repo);

}  // namespace fem::storage
