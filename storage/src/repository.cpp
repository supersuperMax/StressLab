#include "fem/storage/repository.h"

#include "fem/core/io/files.h"
#include "fem/storage/error.h"

#include "names.h"
#include <git2.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <functional>
#include <set>
#include <system_error>
#include <utility>

namespace fem::storage {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view heads_prefix = "refs/heads/";
constexpr std::string_view tags_prefix = "refs/tags/";
constexpr std::string_view stashes_prefix = "refs/stashes/";
constexpr std::string_view results_prefix = "refs/results/";
constexpr std::string_view trailer_prefix = "StressLab-";
constexpr std::string_view service_prefix = ".fem/";
/// Variants and labels are files named by their escaped names: the names are short enough for a
/// project folder in a deep folder of Windows, whose paths libgit2 limits to 260 characters, and
/// the file names stay below the 255 bytes file systems allow.
constexpr std::size_t max_name_characters = 64;
constexpr std::size_t max_reference_bytes = 200;

// --- libgit2 -------------------------------------------------------------------------------------

/// libgit2 for the whole process, set up once. The configuration of the user and of the system
/// (~/.gitconfig, /etc/gitconfig) is not read: the history is a part of the project, and what git
/// is set up to do elsewhere must not change it. Nothing here runs hooks, filters or checkouts, so
/// a repository owned by another user (a project on a shared drive) is safe to open.
class Library {
public:
    Library() {
        git_libgit2_init();
        for (const git_config_level_t level :
             {GIT_CONFIG_LEVEL_PROGRAMDATA, GIT_CONFIG_LEVEL_SYSTEM, GIT_CONFIG_LEVEL_XDG,
              GIT_CONFIG_LEVEL_GLOBAL}) {
            git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, level, "");
        }
        git_libgit2_opts(GIT_OPT_SET_OWNER_VALIDATION, 0);
        git_libgit2_opts(GIT_OPT_ENABLE_FSYNC_GITDIR, 1);
    }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    ~Library() { git_libgit2_shutdown(); }
};

void use_library() {
    static const Library library;
}

template <class T, void (*Free)(T*)>
struct Deleter {
    void operator()(T* pointer) const noexcept { Free(pointer); }
};
template <class T, void (*Free)(T*)>
using Owned = std::unique_ptr<T, Deleter<T, Free>>;

using RepositoryPtr = Owned<git_repository, git_repository_free>;
using ReferencePtr = Owned<git_reference, git_reference_free>;
using ReferenceIteratorPtr = Owned<git_reference_iterator, git_reference_iterator_free>;
using CommitPtr = Owned<git_commit, git_commit_free>;
using TreePtr = Owned<git_tree, git_tree_free>;
using TreeBuilderPtr = Owned<git_treebuilder, git_treebuilder_free>;
using BlobPtr = Owned<git_blob, git_blob_free>;
using ObjectPtr = Owned<git_object, git_object_free>;
using RevwalkPtr = Owned<git_revwalk, git_revwalk_free>;
using SignaturePtr = Owned<git_signature, git_signature_free>;

[[noreturn]] void fail(ProjectErrc code, std::string message) {
    if (const git_error* error = git_error_last(); error != nullptr && error->message != nullptr) {
        message += ": ";
        message += error->message;
    }
    throw ProjectError(code, message);
}

void check(int result, std::string_view what) {
    if (result < 0) {
        fail(ProjectErrc::io, std::string(what));
    }
}

std::string utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::string to_hex(const git_oid& oid) {
    char text[GIT_OID_SHA1_HEXSIZE + 1] = {};
    git_oid_tostr(text, sizeof(text), &oid);
    return text;
}

git_oid to_oid(std::string_view id) {
    git_oid oid;
    if (id.size() != GIT_OID_SHA1_HEXSIZE || git_oid_fromstrn(&oid, id.data(), id.size()) < 0) {
        throw ProjectError(ProjectErrc::unknown_version,
                           "'" + std::string(id) + "' is not the id of a version");
    }
    return oid;
}

bool is_hex(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

using detail::fold_case;

// --- Names ---------------------------------------------------------------------------------------

/// The reference of a variant or a label named `name`.
std::string reference_name(std::string_view prefix, std::string_view name) {
    if (!is_valid_version_name(name)) {
        throw ProjectError(
            ProjectErrc::invalid_name,
            "'" + std::string(name) +
                "' cannot name a variant or a label: it must be UTF-8 text of 1 to " +
                std::to_string(max_name_characters) + " characters without line breaks");
    }
    std::string reference = std::string(prefix) + detail::ref_segment(name);
    int valid = 0;
    if (git_reference_name_is_valid(&valid, reference.c_str()) < 0 || valid == 0) {
        throw ProjectError(ProjectErrc::invalid_name,
                           "'" + std::string(name) + "' cannot name a variant or a label");
    }
    return reference;
}

/// The name of a variant or a label from its reference; none for a reference we did not write.
std::optional<std::string> name_of(std::string_view reference, std::string_view prefix) {
    if (!reference.starts_with(prefix)) {
        return std::nullopt;
    }
    return detail::unescape_segment(reference.substr(prefix.size()));
}

// --- Messages ------------------------------------------------------------------------------------

std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::optional<VersionKind> parse_kind(std::string_view text) {
    for (const VersionKind kind :
         {VersionKind::manual, VersionKind::automatic, VersionKind::restore, VersionKind::stash}) {
        if (to_string(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

/// The message of a commit: the text of the user, then what the history needs as trailers
/// ("StressLab-Kind: automatic"), which `git log` shows as they are and the history reads back.
std::string compose_message(const VersionNote& note) {
    std::string message = trimmed(note.message);
    std::vector<std::pair<std::string_view, std::string>> trailers;
    if (note.kind != VersionKind::manual) {
        trailers.emplace_back("Kind", std::string(to_string(note.kind)));
    }
    if (!note.before.empty()) {
        trailers.emplace_back("Before", note.before);
    }
    if (!note.restored.empty()) {
        trailers.emplace_back("Restored", note.restored);
    }
    if (!note.branch.empty()) {
        trailers.emplace_back("Branch", note.branch);
    }
    if (!trailers.empty()) {
        message += "\n";
        for (const auto& [key, value] : trailers) {
            message += "\n" + std::string(trailer_prefix) + std::string(key) + ": " + value;
        }
    }
    return message + "\n";
}

/// Splits the message of a commit into the text of the user and the trailers of the history.
void parse_message(std::string_view raw, VersionInfo& info) {
    std::string text = trimmed(raw);
    const auto paragraph = text.rfind("\n\n");
    const std::string_view tail = paragraph == std::string::npos
                                      ? std::string_view()
                                      : std::string_view(text).substr(paragraph + 2);
    std::vector<std::pair<std::string, std::string>> trailers;
    bool all_ours = !tail.empty();
    std::size_t start = 0;
    while (all_ours && start <= tail.size()) {
        const auto end = std::min(tail.find('\n', start), tail.size());
        const std::string_view line = tail.substr(start, end - start);
        const auto colon = line.find(": ");
        if (!line.starts_with(trailer_prefix) || colon == std::string_view::npos) {
            all_ours = false;
            break;
        }
        trailers.emplace_back(
            std::string(line.substr(trailer_prefix.size(), colon - trailer_prefix.size())),
            std::string(line.substr(colon + 2)));
        start = end + 1;
    }
    if (all_ours) {
        text = trimmed(std::string_view(text).substr(0, paragraph));
        for (const auto& [key, value] : trailers) {
            if (key == "Kind") {
                info.kind = parse_kind(value).value_or(VersionKind::manual);
            } else if (key == "Before") {
                info.before = value;
            } else if (key == "Restored") {
                info.restored = value;
            } else if (key == "Branch") {
                info.branch = value;
            }
        }
    }
    info.summary = text.substr(0, text.find('\n'));
    info.message = std::move(text);
}

// --- Trees ---------------------------------------------------------------------------------------

/// Files by folder, to build trees from the inside out.
struct Folder {
    std::map<std::string, const std::string*> files;
    std::map<std::string, Folder> folders;
};

void add_file(Folder& root, std::string_view path, const std::string& bytes) {
    Folder* folder = &root;
    std::size_t start = 0;
    for (auto slash = path.find('/'); slash != std::string_view::npos;
         slash = path.find('/', start)) {
        folder = &folder->folders[std::string(path.substr(start, slash - start))];
        start = slash + 1;
    }
    folder->files[std::string(path.substr(start))] = &bytes;
}

}  // namespace

std::string_view to_string(VersionKind kind) noexcept {
    switch (kind) {
        case VersionKind::manual:
            return "manual";
        case VersionKind::automatic:
            return "automatic";
        case VersionKind::restore:
            return "restore";
        case VersionKind::stash:
            return "stash";
    }
    return "manual";
}

bool is_valid_version_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    std::size_t characters = 0;
    // UTF-8 (RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF) without control
    // characters.
    for (std::size_t i = 0; i < name.size();) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if (byte < 0x20 || byte == 0x7F) {
            return false;
        }
        std::size_t length = 1;
        std::uint32_t code = byte;
        if (byte >= 0xF0 && byte <= 0xF4) {
            length = 4;
            code = byte & 0x07U;
        } else if (byte >= 0xE0) {
            length = byte <= 0xEF ? 3 : 0;
            code = byte & 0x0FU;
        } else if (byte >= 0xC2) {
            length = 2;
            code = byte & 0x1FU;
        } else if (byte >= 0x80) {
            length = 0;
        }
        if (length == 0 || i + length > name.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(name[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        if ((length == 3 && (code < 0x800 || (code >= 0xD800 && code <= 0xDFFF))) ||
            (length == 4 && (code < 0x10000 || code > 0x10FFFF))) {
            return false;
        }
        i += length;
        ++characters;
    }
    return characters <= max_name_characters &&
           detail::ref_segment(name).size() <= max_reference_bytes;
}

// --- Repository ----------------------------------------------------------------------------------

struct Repository::State {
    RepositoryPtr repo;

    [[nodiscard]] git_repository* get() const noexcept { return repo.get(); }

    [[nodiscard]] CommitPtr commit(std::string_view id) const {
        const git_oid oid = to_oid(id);
        git_commit* commit = nullptr;
        if (git_commit_lookup(&commit, get(), &oid) < 0) {
            fail(ProjectErrc::unknown_version, "there is no version " + std::string(id));
        }
        return CommitPtr(commit);
    }

    /// The version a reference points at; none if there is no such reference.
    [[nodiscard]] std::optional<std::string> target(const std::string& reference) const {
        git_reference* found = nullptr;
        const int result = git_reference_lookup(&found, get(), reference.c_str());
        if (result == GIT_ENOTFOUND) {
            return std::nullopt;
        }
        check(result, "cannot read the reference " + reference);
        const ReferencePtr owned(found);
        git_reference* resolved = nullptr;
        if (git_reference_resolve(&resolved, found) < 0) {
            return std::nullopt;
        }
        const ReferencePtr owned_resolved(resolved);
        const git_oid* oid = git_reference_target(resolved);
        return oid != nullptr ? std::optional<std::string>(to_hex(*oid)) : std::nullopt;
    }

    /// References matching a glob: full name -> version.
    [[nodiscard]] std::map<std::string, std::string> references(const std::string& glob) const {
        std::map<std::string, std::string> result;
        git_reference_iterator* iterator = nullptr;
        check(git_reference_iterator_glob_new(&iterator, get(), glob.c_str()),
              "cannot list the references");
        const ReferenceIteratorPtr owned(iterator);
        git_reference* reference = nullptr;
        int status = 0;
        while ((status = git_reference_next(&reference, iterator)) == 0) {
            const ReferencePtr owned_reference(reference);
            if (const git_oid* oid = git_reference_target(reference)) {
                result.emplace(git_reference_name(reference), to_hex(*oid));
            }
        }
        if (status != GIT_ITEROVER) {
            check(status, "cannot list the references");
        }
        return result;
    }

    /// Names of the references under `prefix` with their versions, decoded.
    [[nodiscard]] std::map<std::string, std::string> named(std::string_view prefix) const {
        std::map<std::string, std::string> result;
        for (const auto& [reference, id] : references(std::string(prefix) + "*")) {
            if (std::optional<std::string> name = name_of(reference, prefix)) {
                result.emplace(std::move(*name), id);
            }
        }
        return result;
    }

    void create_reference(std::string_view prefix, std::string_view name, std::string_view id,
                          bool create_only) {
        const std::string reference = reference_name(prefix, name);
        const auto exists = [&name] {
            throw ProjectError(ProjectErrc::already_exists,
                               "'" + std::string(name) + "' exists already");
        };
        if (create_only) {
            // The same rule on every file system: names that differ only in case would be one
            // file where the case is ignored.
            const std::string folded = fold_case(name);
            for (const auto& [existing, version] : named(prefix)) {
                if (fold_case(existing) == folded) {
                    exists();
                }
            }
        }
        const git_oid oid = to_oid(id);
        git_reference* created = nullptr;
        const int result = git_reference_create(&created, get(), reference.c_str(), &oid,
                                                create_only ? 0 : 1, nullptr);
        if (result == GIT_EEXISTS) {
            exists();
        }
        check(result, "cannot write the reference " + reference);
        git_reference_free(created);
    }

    [[nodiscard]] std::optional<std::string> id_by_prefix(std::string_view text) const {
        if (!is_hex(text) || text.size() < GIT_OID_MINPREFIXLEN ||
            text.size() > GIT_OID_SHA1_HEXSIZE) {
            return std::nullopt;
        }
        git_oid oid;
        if (git_oid_fromstrn(&oid, text.data(), text.size()) < 0) {
            return std::nullopt;
        }
        git_object* object = nullptr;
        const int result =
            git_object_lookup_prefix(&object, get(), &oid, text.size(), GIT_OBJECT_COMMIT);
        if (result == GIT_EAMBIGUOUS) {
            fail(ProjectErrc::unknown_version,
                 "'" + std::string(text) + "' is the start of several versions");
        }
        if (result < 0) {
            return std::nullopt;
        }
        const ObjectPtr owned(object);
        return to_hex(*git_object_id(object));
    }

    [[nodiscard]] std::optional<std::string> resolve_exact(std::string_view target) const {
        if (is_valid_version_name(target)) {
            if (auto id = target_of_name(heads_prefix, target)) {
                return id;
            }
            if (auto id = target_of_name(tags_prefix, target)) {
                return id;
            }
        }
        return id_by_prefix(target);
    }

    [[nodiscard]] std::optional<std::string> target_of_name(std::string_view prefix,
                                                            std::string_view name) const {
        const std::string reference = std::string(prefix) + detail::ref_segment(name);
        int valid = 0;
        if (git_reference_name_is_valid(&valid, reference.c_str()) < 0 || valid == 0) {
            return std::nullopt;
        }
        return target(reference);
    }

    [[nodiscard]] RevwalkPtr walk() const {
        git_revwalk* walk = nullptr;
        check(git_revwalk_new(&walk, get()), "cannot walk the history");
        RevwalkPtr owned(walk);
        git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
        return owned;
    }
};

Repository::Repository(std::unique_ptr<State> state) : state_(std::move(state)) {}
Repository::Repository(Repository&& other) noexcept = default;
Repository& Repository::operator=(Repository&& other) noexcept = default;
Repository::~Repository() = default;

std::optional<Repository> Repository::open(const fs::path& project_folder) {
    use_library();
    const fs::path folder = project_folder / fs::path(std::u8string(u8".fem/git"));
    std::error_code error;
    if (!fs::is_directory(folder, error)) {
        return std::nullopt;
    }
    git_repository* repo = nullptr;
    if (git_repository_open_bare(&repo, utf8(folder).c_str()) < 0) {
        fail(ProjectErrc::io, "cannot open the history in " + io::display(folder));
    }
    auto state = std::make_unique<State>();
    state->repo.reset(repo);
    return Repository(std::move(state));
}

Repository Repository::open_or_create(const fs::path& project_folder) {
    if (std::optional<Repository> existing = open(project_folder)) {
        return std::move(*existing);
    }
    const fs::path folder = project_folder / fs::path(std::u8string(u8".fem/git"));
    git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    options.flags =
        GIT_REPOSITORY_INIT_BARE | GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_NO_REINIT;
    const std::string head = std::string(main_branch);
    options.initial_head = head.c_str();
    options.description = "The history of a StressLab project";
    git_repository* repo = nullptr;
    if (git_repository_init_ext(&repo, utf8(folder).c_str(), &options) < 0) {
        fail(ProjectErrc::io, "cannot create the history in " + io::display(folder));
    }
    auto state = std::make_unique<State>();
    state->repo.reset(repo);
    return Repository(std::move(state));
}

std::string Repository::commit(const ProjectFiles& files, std::string_view journal,
                               const VersionNote& note, const Author& author,
                               std::span<const std::string> parents) {
    git_repository* repo = state_->get();
    Folder root;
    for (const auto& [path, bytes] : files) {
        add_file(root, path, bytes);
    }
    const std::string journal_text(journal);
    if (!journal_text.empty()) {
        add_file(root, version_journal_file, journal_text);
    }
    const std::function<git_oid(const Folder&)> write_tree = [&](const Folder& folder) {
        git_treebuilder* builder = nullptr;
        check(git_treebuilder_new(&builder, repo, nullptr), "cannot build a tree");
        const TreeBuilderPtr owned(builder);
        for (const auto& [name, bytes] : folder.files) {
            git_oid blob;
            check(git_blob_create_from_buffer(&blob, repo, bytes->data(), bytes->size()),
                  "cannot store the file " + name);
            check(git_treebuilder_insert(nullptr, builder, name.c_str(), &blob, GIT_FILEMODE_BLOB),
                  "cannot add the file " + name);
        }
        for (const auto& [name, inner] : folder.folders) {
            const git_oid tree = write_tree(inner);
            check(git_treebuilder_insert(nullptr, builder, name.c_str(), &tree, GIT_FILEMODE_TREE),
                  "cannot add the folder " + name);
        }
        git_oid tree;
        check(git_treebuilder_write(&tree, builder), "cannot store a tree");
        return tree;
    };
    const git_oid tree_id = write_tree(root);
    git_tree* tree = nullptr;
    check(git_tree_lookup(&tree, repo, &tree_id), "cannot read the tree just stored");
    const TreePtr owned_tree(tree);

    std::vector<CommitPtr> parent_commits;
    std::vector<const git_commit*> parent_pointers;
    for (const std::string& parent : parents) {
        parent_commits.push_back(state_->commit(parent));
        parent_pointers.push_back(parent_commits.back().get());
    }

    git_signature* signature = nullptr;
    check(git_signature_now(&signature, author.name.c_str(), author.email.c_str()),
          "cannot sign the version as '" + author.name + "' <" + author.email + ">");
    const SignaturePtr owned_signature(signature);
    const std::string message = compose_message(note);
    git_oid id;
    check(git_commit_create(&id, repo, nullptr, signature, signature, nullptr, message.c_str(),
                            tree, parent_pointers.size(), parent_pointers.data()),
          "cannot store the version");
    return to_hex(id);
}

VersionFiles Repository::read(std::string_view id) const {
    const CommitPtr commit = state_->commit(id);
    git_tree* tree = nullptr;
    check(git_commit_tree(&tree, commit.get()), "cannot read the version " + std::string(id));
    const TreePtr owned(tree);

    struct Walk {
        git_repository* repo;
        VersionFiles result;
        std::string failed;
    } walk{state_->get(), {}, {}};
    const auto visit = [](const char* root, const git_tree_entry* entry, void* payload) -> int {
        auto& state = *static_cast<Walk*>(payload);
        if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB) {
            return 0;
        }
        const std::string path = std::string(root) + git_tree_entry_name(entry);
        git_blob* blob = nullptr;
        if (git_blob_lookup(&blob, state.repo, git_tree_entry_id(entry)) < 0) {
            state.failed = path;
            return -1;
        }
        const BlobPtr owned_blob(blob);
        std::string bytes(static_cast<const char*>(git_blob_rawcontent(blob)),
                          static_cast<std::size_t>(git_blob_rawsize(blob)));
        if (path == version_journal_file) {
            state.result.journal = std::move(bytes);
        } else if (!path.starts_with(service_prefix)) {
            state.result.files.emplace(path, std::move(bytes));
        }
        return 0;
    };
    if (git_tree_walk(tree, GIT_TREEWALK_PRE, visit, &walk) < 0) {
        fail(ProjectErrc::io, "cannot read " + walk.failed + " of the version " + std::string(id));
    }
    return std::move(walk.result);
}

std::optional<std::string> Repository::read_file(std::string_view id, std::string_view path) const {
    const CommitPtr commit = state_->commit(id);
    git_tree* tree = nullptr;
    check(git_commit_tree(&tree, commit.get()), "cannot read the version " + std::string(id));
    const TreePtr owned(tree);
    git_tree_entry* entry = nullptr;
    const std::string key(path);
    const int result = git_tree_entry_bypath(&entry, tree, key.c_str());
    if (result == GIT_ENOTFOUND) {
        return std::nullopt;
    }
    check(result, "cannot find " + key + " in the version " + std::string(id));
    const Owned<git_tree_entry, git_tree_entry_free> owned_entry(entry);
    if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB) {
        return std::nullopt;
    }
    git_blob* blob = nullptr;
    check(git_blob_lookup(&blob, state_->get(), git_tree_entry_id(entry)),
          "cannot read " + key + " of the version " + std::string(id));
    const BlobPtr owned_blob(blob);
    return std::string(static_cast<const char*>(git_blob_rawcontent(blob)),
                       static_cast<std::size_t>(git_blob_rawsize(blob)));
}

VersionInfo Repository::info(std::string_view id) const {
    const CommitPtr commit = state_->commit(id);
    VersionInfo info;
    info.id = to_hex(*git_commit_id(commit.get()));
    for (unsigned int i = 0; i < git_commit_parentcount(commit.get()); ++i) {
        info.parents.push_back(to_hex(*git_commit_parent_id(commit.get(), i)));
    }
    const char* message = git_commit_message(commit.get());
    parse_message(message != nullptr ? message : "", info);
    if (const git_signature* author = git_commit_author(commit.get())) {
        info.author = {author->name != nullptr ? author->name : "",
                       author->email != nullptr ? author->email : ""};
        info.time = author->when.time;
        info.time_offset = author->when.offset;
    }
    return info;
}

std::vector<VersionInfo> Repository::log(std::span<const std::string> from,
                                         std::size_t limit) const {
    const RevwalkPtr walk = state_->walk();
    for (const std::string& id : from) {
        const git_oid oid = to_oid(id);
        check(git_revwalk_push(walk.get(), &oid), "cannot walk the history from " + id);
    }
    std::vector<VersionInfo> result;
    git_oid oid;
    while ((limit == 0 || result.size() < limit) && git_revwalk_next(&oid, walk.get()) == 0) {
        result.push_back(info(to_hex(oid)));
    }
    return result;
}

std::vector<std::string> Repository::reachable() const {
    std::set<std::string> starts;
    for (const std::string_view prefix : {heads_prefix, tags_prefix, stashes_prefix}) {
        for (const auto& [reference, id] : state_->references(std::string(prefix) + "*")) {
            starts.insert(id);
        }
    }
    if (const std::optional<std::string> head = this->head().version) {
        starts.insert(*head);
    }
    const RevwalkPtr walk = state_->walk();
    for (const std::string& id : starts) {
        const git_oid oid = to_oid(id);
        check(git_revwalk_push(walk.get(), &oid), "cannot walk the history from " + id);
    }
    std::vector<std::string> result;
    git_oid oid;
    while (git_revwalk_next(&oid, walk.get()) == 0) {
        result.push_back(to_hex(oid));
    }
    return result;
}

std::string Repository::resolve(std::string_view target) const {
    if (std::optional<std::string> id = state_->resolve_exact(target)) {
        return *id;
    }
    // "main~2": two versions back along the first parents.
    const auto tilde = target.rfind('~');
    unsigned long steps = 0;
    if (tilde != std::string_view::npos && tilde > 0 && tilde + 1 < target.size()) {
        const char* first = target.data() + tilde + 1;
        const char* last = target.data() + target.size();
        const auto [end, error] = std::from_chars(first, last, steps);
        if (error == std::errc() && end == last) {
            std::string id = resolve(target.substr(0, tilde));
            for (unsigned long i = 0; i < steps; ++i) {
                const CommitPtr commit = state_->commit(id);
                if (git_commit_parentcount(commit.get()) == 0) {
                    throw ProjectError(
                        ProjectErrc::unknown_version,
                        "'" + std::string(target) + "' goes back past the first version");
                }
                id = to_hex(*git_commit_parent_id(commit.get(), 0));
            }
            return id;
        }
    }
    throw ProjectError(ProjectErrc::unknown_version,
                       "there is no variant, label or version '" + std::string(target) + "'");
}

Head Repository::head() const {
    git_reference* found = nullptr;
    check(git_reference_lookup(&found, state_->get(), "HEAD"), "cannot read HEAD");
    const ReferencePtr owned(found);
    Head head;
    if (git_reference_type(found) == GIT_REFERENCE_SYMBOLIC) {
        const std::string target = git_reference_symbolic_target(found);
        head.branch = name_of(target, heads_prefix);
        head.version = state_->target(target);
    } else if (const git_oid* oid = git_reference_target(found)) {
        head.version = to_hex(*oid);
    }
    return head;
}

void Repository::attach_head(std::string_view branch) {
    const std::string reference = reference_name(heads_prefix, branch);
    check(git_repository_set_head(state_->get(), reference.c_str()), "cannot move HEAD");
}

void Repository::detach_head(std::string_view id) {
    const git_oid oid = to_oid(id);
    check(git_repository_set_head_detached(state_->get(), &oid), "cannot move HEAD");
}

std::map<std::string, std::string> Repository::branches() const {
    return state_->named(heads_prefix);
}

std::optional<std::string> Repository::branch(std::string_view name) const {
    return state_->target(reference_name(heads_prefix, name));
}

void Repository::set_branch(std::string_view name, std::string_view id, bool create_only) {
    state_->create_reference(heads_prefix, name, id, create_only);
}

std::map<std::string, std::string> Repository::tags() const {
    return state_->named(tags_prefix);
}

void Repository::create_tag(std::string_view name, std::string_view id) {
    state_->create_reference(tags_prefix, name, id, true);
}

std::vector<std::string> Repository::stashes() const {
    // References are numbered with ten digits, so their names sort as the numbers do.
    const std::map<std::string, std::string> all =
        state_->references(std::string(stashes_prefix) + "*");
    std::vector<std::string> result;
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        result.push_back(it->second);
    }
    return result;
}

void Repository::push_stash(std::string_view id) {
    const std::map<std::string, std::string> all =
        state_->references(std::string(stashes_prefix) + "*");
    unsigned long next = 1;
    if (!all.empty()) {
        const std::string_view last =
            std::string_view(all.rbegin()->first).substr(stashes_prefix.size());
        unsigned long number = 0;
        (void)std::from_chars(last.data(), last.data() + last.size(), number);
        next = number + 1;
    }
    char name[16] = {};
    std::snprintf(name, sizeof(name), "%010lu", next);
    const std::string reference = std::string(stashes_prefix) + name;
    const git_oid oid = to_oid(id);
    git_reference* created = nullptr;
    check(git_reference_create(&created, state_->get(), reference.c_str(), &oid, 0, nullptr),
          "cannot put the changes aside");
    git_reference_free(created);
}

void Repository::drop_stash(std::size_t index) {
    const std::map<std::string, std::string> all =
        state_->references(std::string(stashes_prefix) + "*");
    if (index >= all.size()) {
        throw ProjectError(ProjectErrc::unknown_version,
                           "there are " + std::to_string(all.size()) +
                               " sets of changes put aside, no number " + std::to_string(index));
    }
    auto it = all.rbegin();
    std::advance(it, static_cast<std::ptrdiff_t>(index));
    git_reference* found = nullptr;
    check(git_reference_lookup(&found, state_->get(), it->first.c_str()),
          "cannot read the changes put aside");
    const ReferencePtr owned(found);
    check(git_reference_delete(found), "cannot forget the changes put aside");
}

namespace {

/// The reference of the pinned result of `key`. Throws ProjectError(unknown_result) for a key
/// that is not 64 lower-case hex digits.
std::string pin_reference(std::string_view key) {
    if (key.size() != 64 || !std::all_of(key.begin(), key.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        throw ProjectError(ProjectErrc::unknown_result,
                           "'" + std::string(key) + "' is not the key of a result");
    }
    return std::string(results_prefix) + std::string(key);
}

std::string read_blob(git_repository* repo, const git_oid& oid, const std::string& what) {
    git_blob* blob = nullptr;
    check(git_blob_lookup(&blob, repo, &oid), "cannot read " + what);
    const BlobPtr owned(blob);
    return {static_cast<const char*>(git_blob_rawcontent(blob)),
            static_cast<std::size_t>(git_blob_rawsize(blob))};
}

}  // namespace

std::map<std::string, std::string> Repository::pins() const {
    std::map<std::string, std::string> result;
    for (const auto& [reference, id] : state_->references(std::string(results_prefix) + "*")) {
        const git_oid oid = to_oid(id);
        result.emplace(reference.substr(results_prefix.size()),
                       read_blob(state_->get(), oid, "the pinned result " + reference));
    }
    return result;
}

std::optional<std::string> Repository::pin(std::string_view key) const {
    const std::string reference = pin_reference(key);
    git_reference* found = nullptr;
    const int result = git_reference_lookup(&found, state_->get(), reference.c_str());
    if (result == GIT_ENOTFOUND) {
        return std::nullopt;
    }
    check(result, "cannot read the reference " + reference);
    const ReferencePtr owned(found);
    const git_oid* oid = git_reference_target(found);
    if (oid == nullptr) {
        return std::nullopt;
    }
    return read_blob(state_->get(), *oid, "the pinned result " + reference);
}

void Repository::set_pin(std::string_view key, std::string_view record) {
    const std::string reference = pin_reference(key);
    git_oid oid;
    check(git_blob_create_from_buffer(&oid, state_->get(), record.data(), record.size()),
          "cannot store the pinned result " + std::string(key));
    git_reference* created = nullptr;
    check(git_reference_create(&created, state_->get(), reference.c_str(), &oid, 1, nullptr),
          "cannot write the reference " + reference);
    git_reference_free(created);
}

bool Repository::remove_pin(std::string_view key) {
    const std::string reference = pin_reference(key);
    git_reference* found = nullptr;
    const int result = git_reference_lookup(&found, state_->get(), reference.c_str());
    if (result == GIT_ENOTFOUND) {
        return false;
    }
    check(result, "cannot read the reference " + reference);
    const ReferencePtr owned(found);
    check(git_reference_delete(found), "cannot forget the pinned result " + std::string(key));
    return true;
}

void Repository::adopt(const fs::path& project_folder) {
    use_library();
    // The configuration a new history gets here, probed by libgit2 on the same file system (case,
    // links, modes): initialising the unpacked repository again would not probe the case.
    const fs::path folder = project_folder / fs::path(std::u8string(u8".fem/git"));
    const fs::path probe = project_folder / fs::path(std::u8string(u8".fem/git-probe"));
    std::error_code ignored;
    fs::remove_all(probe, ignored);
    git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    options.flags = GIT_REPOSITORY_INIT_BARE | GIT_REPOSITORY_INIT_MKPATH;
    git_repository* repo = nullptr;
    const int result = git_repository_init_ext(&repo, utf8(probe).c_str(), &options);
    git_repository_free(repo);
    const std::optional<std::string> config =
        result < 0 ? std::nullopt : io::read_file(probe / "config");
    fs::remove_all(probe, ignored);
    if (!config || !io::write_file(folder / "config", *config)) {
        fail(ProjectErrc::io, "cannot set up the history in " + io::display(folder));
    }
}

}  // namespace fem::storage
