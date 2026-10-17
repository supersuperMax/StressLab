#include "fem/storage/history.h"

#include "fem/core/command/journal.h"
#include "fem/core/command/python_literal.h"
#include "fem/core/io/files.h"
#include "fem/core/model/error.h"
#include "fem/core/model/object.h"
#include "fem/storage/blob_store.h"
#include "fem/storage/error.h"
#include "fem/storage/object_data_codec.h"
#include "fem/storage/project.h"
#include "fem/storage/result_cache.h"
#include "fem/storage/session.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <system_error>
#include <utility>

namespace fem::storage {

namespace {

namespace fs = std::filesystem;
using model::ObjectId;

fs::path to_path(std::string_view utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

fs::path objects_folder(const fs::path& folder) {
    return folder / fs::path(u8".fem/objects");
}

fs::path journal_path(const fs::path& folder) {
    return folder / to_path(version_journal_file);
}

std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::string short_id(std::string_view id) {
    return std::string(id.substr(0, 7));
}

/// `stresslab.history.<function>(<arguments>)`.
std::string call(std::string_view function, std::string_view arguments = {}) {
    return "stresslab.history." + std::string(function) + "(" + std::string(arguments) + ")";
}

std::string quoted(std::string_view text) {
    return command::python_string(text);
}

/// The entries of a journal script: what follows its header, as one entry.
std::vector<std::string> journal_entries(std::string_view text) {
    const std::string header = command::journal_header();
    if (text.starts_with(header)) {
        text.remove_prefix(header.size());
    }
    while (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return {};
    }
    return {std::string(text)};
}

/// project.json without the versions of the modules: a newer version of an installed module is
/// not a change of the model.
std::string without_module_versions(const std::string& text) {
    nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        return text;
    }
    value.erase("modules");
    return value.dump();
}

bool same_model(const ProjectFiles& a, const ProjectFiles& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (auto left = a.begin(), right = b.begin(); left != a.end(); ++left, ++right) {
        if (left->first != right->first) {
            return false;
        }
        if (left->second != right->second &&
            (left->first != project_file ||
             without_module_versions(left->second) != without_module_versions(right->second))) {
            return false;
        }
    }
    return true;
}

/// The user of the system, as the default author of versions.
Author default_author() {
    std::string name;
#ifdef _WIN32
    wchar_t* value = nullptr;
    std::size_t size = 0;
    if (_wdupenv_s(&value, &size, L"USERNAME") == 0 && value != nullptr) {
        const std::u8string text = fs::path(value).u8string();
        name.assign(text.begin(), text.end());
    }
    std::free(value);
#else
    for (const char* variable : {"USER", "LOGNAME"}) {
        const char* value = std::getenv(variable);  // NOLINT(concurrency-mt-unsafe)
        if (value != nullptr && *value != '\0') {
            name = value;
            break;
        }
    }
#endif
    name.erase(std::remove_if(name.begin(), name.end(),
                              [](char c) { return c == '<' || c == '>' || c == '\n'; }),
               name.end());
    if (trimmed(name).empty()) {
        name = "StressLab";
    }
    // libgit2 wants an e-mail; the application has no setting for it yet.
    return {trimmed(name), "unknown"};
}

}  // namespace

ProjectHistory::ProjectHistory(ProjectSession& session, command::CommandBus& bus)
    : session_(session), bus_(bus), author_(default_author()) {
    journal_ = bus_.on_journal([this](std::string_view entry) { pending_.emplace_back(entry); });
    before_command_ = bus_.on_before_command([this](const command::CommandSpec& spec) {
        if (spec.snapshot_before) {
            snapshot_before(spec);
        }
    });
    bus_.register_command(
        {.id = std::string(restore_object_command),
         .owner = std::string(owner),
         .args = {{.name = "object", .type = command::ArgType::object},
                  {.name = "version", .type = command::ArgType::string}}},
        [this](command::CommandContext& context) { return restore_object(context); });
    bus_.register_command(
        {.id = std::string(restore_objects_command),
         .owner = std::string(owner),
         .args = {{.name = "version", .type = command::ArgType::string},
                  {.name = "types", .type = command::ArgType::list, .required = false},
                  {.name = "objects", .type = command::ArgType::list, .required = false}}},
        [this](command::CommandContext& context) { return restore_objects(context); });
}

ProjectHistory::~ProjectHistory() {
    (void)bus_.unregister_command(restore_object_command);
    (void)bus_.unregister_command(restore_objects_command);
}

// --- What there is -------------------------------------------------------------------------------

std::optional<std::string> ProjectHistory::current_branch() const {
    if (const std::optional<Repository> repo = repository()) {
        return repo->head().branch;
    }
    return std::string(main_branch);
}

std::optional<std::string> ProjectHistory::current_version() const {
    if (const std::optional<Repository> repo = repository()) {
        return repo->head().version;
    }
    return std::nullopt;
}

bool ProjectHistory::has_changes() const {
    const std::optional<Repository> repo = repository();
    return differs(repo ? &*repo : nullptr, current_files());
}

std::vector<VersionInfo> ProjectHistory::log(std::optional<std::string_view> target, bool all,
                                             std::size_t limit) const {
    const std::optional<Repository> repo = repository();
    if (!repo) {
        if (target) {
            throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
        }
        return {};
    }
    std::set<std::string> from;
    if (all) {
        for (const auto& [name, id] : repo->branches()) {
            from.insert(id);
        }
        for (const auto& [name, id] : repo->tags()) {
            from.insert(id);
        }
    }
    if (target) {
        from.insert(repo->resolve(*target));
    } else if (const std::optional<std::string> head = repo->head().version) {
        from.insert(*head);
    }
    const std::vector<std::string> starts(from.begin(), from.end());
    return repo->log(starts, limit);
}

VersionInfo ProjectHistory::info(std::string_view target) const {
    const std::optional<Repository> repo = repository();
    if (!repo) {
        throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
    }
    return repo->info(repo->resolve(target));
}

std::string ProjectHistory::journal(std::string_view target) const {
    const std::optional<Repository> repo = repository();
    if (!repo) {
        throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
    }
    return repo->read(repo->resolve(target)).journal;
}

std::string ProjectHistory::resolve(std::string_view target) const {
    const std::optional<Repository> repo = repository();
    if (!repo) {
        throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
    }
    return repo->resolve(target);
}

std::map<std::string, std::string> ProjectHistory::branches() const {
    const std::optional<Repository> repo = repository();
    return repo ? repo->branches() : std::map<std::string, std::string>{};
}

std::map<std::string, std::string> ProjectHistory::tags() const {
    const std::optional<Repository> repo = repository();
    return repo ? repo->tags() : std::map<std::string, std::string>{};
}

std::vector<VersionInfo> ProjectHistory::stashes() const {
    std::vector<VersionInfo> result;
    if (const std::optional<Repository> repo = repository()) {
        for (const std::string& id : repo->stashes()) {
            result.push_back(repo->info(id));
        }
    }
    return result;
}

ModelDiff ProjectHistory::diff(std::string_view from, std::optional<std::string_view> to) const {
    const std::optional<Repository> repo = repository();
    if (!repo) {
        throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
    }
    const std::string before = repo->resolve(from);
    if (to) {
        const std::string after = repo->resolve(*to);
        return diff_files(repo->read(before).files, repo->read(after).files);
    }
    return diff_files(repo->read(before).files, current_files());
}

std::vector<std::string> ProjectHistory::blob_roots() const {
    const std::optional<Repository> repo = repository();
    return repo ? history_blob_roots(*repo) : std::vector<std::string>{};
}

std::vector<std::string> history_blob_roots(const Repository& repo) {
    // Versions share their refs/blobs.json while the bulk data does not change: read each once.
    std::set<std::string> seen;
    std::set<std::string> roots;
    for (std::string& root : pinned_result_roots(repo)) {
        roots.insert(std::move(root));
    }
    for (const std::string& id : repo.reachable()) {
        std::optional<std::string> refs = repo.read_file(id, blob_refs_file);
        std::optional<std::string> project = repo.read_file(id, project_file);
        if (!refs || !project || !seen.insert(*refs).second) {
            continue;
        }
        ProjectFiles files;
        files.emplace(std::string(project_file), std::move(*project));
        files.emplace(std::string(blob_refs_file), std::move(*refs));
        for (std::string& root : read_blob_roots(files)) {
            roots.insert(std::move(root));
        }
    }
    return {roots.begin(), roots.end()};
}

// --- Actions -------------------------------------------------------------------------------------

std::string ProjectHistory::commit(std::string_view message) {
    check_idle("committing a version");
    const std::string text = trimmed(message);
    if (text.empty()) {
        throw ProjectError(ProjectErrc::invalid_name, "a version needs a message");
    }
    Repository repo = repository_for_writing();
    const Head head = repo.head();
    if (!head.branch) {
        throw ProjectError(ProjectErrc::detached,
                           "a version is viewed on its own: make a variant from it to commit");
    }
    const ProjectFiles files = current_files();
    if (head.version && !differs(&repo, files)) {
        throw ProjectError(ProjectErrc::nothing_to_commit,
                           "the model is the same as the version " + short_id(*head.version));
    }
    (void)write_project_files(folder(), files);
    const std::string id = make_version(repo, files, pending_journal(),
                                        {.message = text, .kind = VersionKind::manual});
    bus_.journal_action(call("commit", quoted(message)));
    forget_pending();
    session_.set_state(session_.folder_, false);
    return id;
}

std::string ProjectHistory::snapshot(std::string_view reason) {
    return snapshot(reason, {}, true);
}

void ProjectHistory::snapshot_before(const command::CommandSpec& spec) {
    if (!session_.folder_) {
        return;  // Nowhere to keep a version.
    }
    if (const std::optional<Repository> repo = repository(); repo && !repo->head().branch) {
        return;  // A version viewed on its own: there is no variant to add to.
    }
    (void)snapshot("Before " + spec.id, spec.id, false);
}

std::string ProjectHistory::snapshot(std::string_view reason, std::string_view before,
                                     bool journaled) {
    check_idle("making a version");
    Repository repo = repository_for_writing();
    const Head head = repo.head();
    const ProjectFiles files = current_files();
    std::string id;
    if (head.version && !differs(&repo, files)) {
        id = *head.version;
    } else {
        if (!head.branch) {
            throw ProjectError(ProjectErrc::detached,
                               "a version is viewed on its own: make a variant from it first");
        }
        (void)write_project_files(folder(), files);
        const std::string text = trimmed(reason);
        id = make_version(repo, files, pending_journal(),
                          {.message = text.empty() ? std::string("Automatic version") : text,
                           .kind = VersionKind::automatic,
                           .before = std::string(before)});
    }
    if (journaled) {
        bus_.journal_action(call("snapshot", quoted(reason)));
    }
    if (id != head.version) {
        forget_pending();
        session_.set_state(session_.folder_, false);
    }
    return id;
}

void ProjectHistory::branch(std::string_view name, std::optional<std::string_view> at) {
    check_idle("making a variant");
    Repository repo = repository_for_writing();
    std::string id;
    if (at) {
        id = repo.resolve(*at);
    } else if (const std::optional<std::string> head = repo.head().version) {
        id = *head;
    } else {
        throw ProjectError(ProjectErrc::unknown_version,
                           "there are no versions yet: commit one to make a variant from it");
    }
    repo.set_branch(name, id, true);
    bus_.journal_action(call("branch", quoted(name) + (at ? ", at=" + quoted(*at) : "")));
}

void ProjectHistory::checkout(std::string_view target) {
    check_idle("switching to another version");
    std::optional<Repository> found = repository();
    if (!found) {
        throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
    }
    Repository& repo = *found;
    std::optional<std::string> branch_id;
    if (is_valid_version_name(target)) {
        branch_id = repo.branch(target);
    }
    const std::string id = branch_id ? *branch_id : repo.resolve(target);
    if (has_changes()) {
        throw ProjectError(ProjectErrc::uncommitted_changes,
                           "the model has changes: commit them or put them aside first");
    }
    (void)load(repo.read(id), call("checkout", quoted(target)));
    if (branch_id) {
        repo.attach_head(target);
    } else {
        repo.detach_head(id);
    }
    forget_pending();
    session_.set_state(session_.folder_, false);
}

void ProjectHistory::tag(std::string_view name, std::optional<std::string_view> at) {
    check_idle("making a label");
    Repository repo = repository_for_writing();
    std::string id;
    if (at) {
        id = repo.resolve(*at);
    } else if (const std::optional<std::string> head = repo.head().version) {
        id = *head;
    } else {
        throw ProjectError(ProjectErrc::unknown_version,
                           "there are no versions yet: commit one to label it");
    }
    repo.create_tag(name, id);
    bus_.journal_action(call("tag", quoted(name) + (at ? ", at=" + quoted(*at) : "")));
}

void ProjectHistory::stash(std::string_view message) {
    check_idle("putting changes aside");
    std::optional<Repository> found = repository();
    const Head head = found ? found->head() : Head{};
    if (!head.version) {
        throw ProjectError(ProjectErrc::unknown_version,
                           "there are no versions yet to bring back: commit one first");
    }
    Repository& repo = *found;
    const ProjectFiles files = current_files();
    if (!differs(&repo, files)) {
        throw ProjectError(ProjectErrc::nothing_to_stash, "the model has no changes to put aside");
    }
    const std::string text = trimmed(message);
    const std::string id =
        repo.commit(files, pending_journal(),
                    {.message = text.empty() ? std::string("Changes put aside") : text,
                     .kind = VersionKind::stash,
                     .branch = head.branch.value_or(std::string())},
                    author_, std::vector<std::string>{*head.version});
    repo.push_stash(id);
    (void)load(repo.read(*head.version),
               call("stash", message.empty() ? std::string() : quoted(message)));
    forget_pending();
    session_.set_state(session_.folder_, false);
}

void ProjectHistory::stash_pop(std::size_t index) {
    check_idle("bringing back changes put aside");
    std::optional<Repository> found = repository();
    const std::vector<std::string> stashes = found ? found->stashes() : std::vector<std::string>{};
    if (index >= stashes.size()) {
        throw ProjectError(ProjectErrc::unknown_version,
                           "there are " + std::to_string(stashes.size()) +
                               " sets of changes put aside, no number " + std::to_string(index));
    }
    Repository& repo = *found;
    if (has_changes()) {
        throw ProjectError(ProjectErrc::uncommitted_changes,
                           "the model has changes: commit them or put them aside first");
    }
    const VersionInfo stashed = repo.info(stashes[index]);
    if (stashed.parents.empty() || repo.head().version != stashed.parents.front()) {
        throw ProjectError(ProjectErrc::stash_conflict,
                           "the changes were put aside on the version " +
                               short_id(stashed.parents.empty() ? "" : stashed.parents.front()) +
                               ", not on the current one: bringing them over would be a merge");
    }
    const VersionFiles version = repo.read(stashed.id);
    (void)load(version, call("stash_pop", index == 0 ? "" : std::to_string(index)));
    repo.drop_stash(index);
    // The journal of the changes comes back with them.
    pending_ = journal_entries(version.journal);
    saved(folder());
    session_.set_state(session_.folder_, false);
}

void ProjectHistory::stash_drop(std::size_t index) {
    check_idle("forgetting changes put aside");
    std::optional<Repository> found = repository();
    if (!found) {
        throw ProjectError(ProjectErrc::unknown_version, "there are no changes put aside");
    }
    found->drop_stash(index);
    bus_.journal_action(call("stash_drop", index == 0 ? "" : std::to_string(index)));
}

std::string ProjectHistory::restore_version(std::string_view target) {
    check_idle("restoring a version");
    std::optional<Repository> found = repository();
    if (!found) {
        throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
    }
    Repository& repo = *found;
    if (!repo.head().branch) {
        throw ProjectError(ProjectErrc::detached,
                           "a version is viewed on its own: make a variant from it first");
    }
    const std::string id = repo.resolve(target);
    // Changes not in the history are kept as a version of their own first.
    if (has_changes()) {
        (void)snapshot("Before restoring version " + short_id(id), {}, false);
    }
    const VersionInfo restored = repo.info(id);
    const std::uint64_t next_id = bus_.document().next_id();
    const ProjectFiles files =
        load(repo.read(id), call("restore_version", quoted(target)), next_id);
    const Head head = repo.head();
    std::string result;
    if (head.version && !differs(&repo, files)) {
        result = *head.version;
    } else {
        result =
            make_version(repo, files, pending_journal(),
                         {.message = "Restored version " + short_id(id) + ": " + restored.summary,
                          .kind = VersionKind::restore,
                          .restored = id});
    }
    forget_pending();
    session_.set_state(session_.folder_, false);
    return result;
}

void ProjectHistory::set_author(Author author) {
    author_ = std::move(author);
}

// --- Restoring objects ---------------------------------------------------------------------------

namespace {

/// A store that keeps nothing: the reference data would have, without writing it.
class HashingBlobStore final : public BlobStore {
public:
    std::string put(std::span<const std::byte> bytes) override { return blob_ref(bytes); }
    [[nodiscard]] Bytes get(std::string_view ref) const override {
        throw ProjectError(ProjectErrc::missing_blob, "the block is not kept: " + std::string(ref));
    }
    [[nodiscard]] bool contains(std::string_view /*ref*/) const override { return false; }
};

/// The object of the model is as the version keeps it.
bool same_as(const model::Object& now, const ObjectRecord& record) {
    const model::NewObject& old = record.object.init;
    if (now.owner() != old.owner || now.type() != old.type ||
        now.schema_version() != old.schema_version || now.name() != old.name ||
        now.parent() != old.parent || now.properties().dump() != old.properties.dump()) {
        return false;
    }
    std::vector<ObjectId> dependencies = old.dependencies;
    std::sort(dependencies.begin(), dependencies.end());
    if (!std::equal(dependencies.begin(), dependencies.end(), now.dependencies().begin(),
                    now.dependencies().end())) {
        return false;
    }
    if ((now.data() != nullptr) != !record.blob.empty()) {
        return false;
    }
    HashingBlobStore hashes;
    return now.data() == nullptr || put_object_data(*now.data(), hashes) == record.blob;
}

/// Makes an object of the model be as `stored`, an object of a version with the same id: the
/// same object, back after its removal or in place of what it is now.
void put_back(model::DocumentEditor& editor, model::StoredObject stored) {
    const ObjectId id = stored.id;
    const model::Document& document = editor.document();
    if (!document.contains(id)) {
        editor.restore(std::move(stored));
        return;
    }
    const model::Object& now = document.get(id);
    model::NewObject& old = stored.init;
    if (now.type() != old.type || now.owner() != old.owner) {
        throw model::ModelError(model::ModelErrc::invalid_argument,
                                "the object of the version is of another type", id);
    }
    if (now.schema_version() != old.schema_version) {
        throw model::ModelError(model::ModelErrc::invalid_argument,
                                "the version keeps the object in schema version " +
                                    std::to_string(old.schema_version) + ", the model in " +
                                    std::to_string(now.schema_version()),
                                id);
    }
    if (now.parent() != old.parent) {
        // Objects do not move in the tree; only a hand-edited file could make this.
        throw model::ModelError(model::ModelErrc::invalid_argument,
                                "the object has another parent in the version", id);
    }
    editor.rename(id, std::move(old.name));
    editor.set_properties(id, std::move(old.properties));
    if (old.data != nullptr || now.data() != nullptr) {
        editor.set_data(id, std::move(old.data));
    }
    const std::vector<ObjectId> current(now.dependencies().begin(), now.dependencies().end());
    for (const ObjectId target_id : current) {
        if (std::find(old.dependencies.begin(), old.dependencies.end(), target_id) ==
            old.dependencies.end()) {
            (void)editor.remove_dependency(id, target_id);
        }
    }
    for (const ObjectId target_id : old.dependencies) {
        (void)editor.add_dependency(id, target_id);
    }
}

}  // namespace

command::CommandResult ProjectHistory::restore_object(command::CommandContext& context) {
    const ObjectId id = context.args().object("object");
    const std::string& target = context.args().string("version");
    const std::optional<Repository> repo = repository();
    if (!repo) {
        throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
    }
    const std::string version = repo->resolve(target);
    const FolderBlobStore blobs(objects_folder(folder()));
    std::optional<model::StoredObject> stored = read_object(repo->read(version).files, blobs, id);
    if (!stored) {
        throw model::ModelError(model::ModelErrc::unknown_object,
                                "the version " + short_id(version) + " has no such object", id);
    }
    put_back(context.editor(), std::move(*stored));
    return command::Value(id);
}

command::CommandResult ProjectHistory::restore_objects(command::CommandContext& context) {
    const command::Arguments& args = context.args();
    const std::string& target = args.string("version");
    std::vector<std::string> types;
    if (args.has("types")) {
        for (const command::Value& type : args.list("types")) {
            types.push_back(type.as_string());
        }
    }
    const std::vector<ObjectId> named =
        args.has("objects") ? args.objects("objects") : std::vector<ObjectId>{};
    if (types.empty() && named.empty()) {
        throw model::ModelError(model::ModelErrc::invalid_argument,
                                "name the types or the objects to restore");
    }
    const std::optional<Repository> repo = repository();
    if (!repo) {
        throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
    }
    const std::string version = repo->resolve(target);
    const ProjectFiles files = repo->read(version).files;
    const ProjectRecords records = read_project_records(files);
    const model::Document& document = context.document();
    model::DocumentEditor& editor = context.editor();

    // "material" is every object of that type, "fem.model.structural/material" those of one owner.
    const auto chosen = [&](ObjectId id, const std::string& object_owner, const std::string& type) {
        if (std::find(named.begin(), named.end(), id) != named.end()) {
            return true;
        }
        return std::any_of(types.begin(), types.end(), [&](const std::string& selector) {
            return selector == type || selector == object_owner + '/' + type;
        });
    };
    for (const ObjectId id : named) {
        if (!document.contains(id) && !records.objects.contains(id)) {
            throw model::ModelError(
                model::ModelErrc::unknown_object,
                "neither the model nor the version " + short_id(version) + " has the object", id);
        }
    }
    std::vector<ObjectId> back;     // Removed since the version: brought back, parents first.
    std::vector<ObjectId> changed;  // In both and different: made as in the version.
    std::vector<ObjectId> added;    // Created since the version: removed, children first.
    for (const auto& [id, record] : records.objects) {
        const model::NewObject& init = record.object.init;
        if (!chosen(id, init.owner, init.type)) {
            continue;
        }
        if (!document.contains(id)) {
            back.push_back(id);
        } else if (!same_as(document.get(id), record)) {
            changed.push_back(id);
        }
    }
    for (const model::Object& object : document.objects()) {
        if (!records.objects.contains(object.id()) &&
            chosen(object.id(), object.owner(), object.type())) {
            added.push_back(object.id());
        }
    }
    std::sort(back.begin(), back.end());
    std::sort(added.begin(), added.end());

    const FolderBlobStore blobs(objects_folder(folder()));
    const auto stored = [&](ObjectId id) {
        std::optional<model::StoredObject> object = read_object(files, blobs, id);
        return std::move(object.value());
    };
    // A parent is older than its children, so it has a smaller id and comes back first.
    for (const ObjectId id : back) {
        model::StoredObject object = stored(id);
        const ObjectId parent = object.init.parent;
        if (parent.is_valid() && !document.contains(parent)) {
            throw model::ModelError(model::ModelErrc::unknown_object,
                                    "the parent " + model::to_string(parent) +
                                        " of the object is not in the model: restore it too",
                                    id);
        }
        put_back(editor, std::move(object));
    }
    for (const ObjectId id : changed) {
        put_back(editor, stored(id));
    }
    for (auto it = added.rbegin(); it != added.rend(); ++it) {
        if (!document.contains(*it)) {
            continue;  // Removed with its parent.
        }
        if (!editor.remove(*it).ok()) {
            throw model::ModelError(model::ModelErrc::removal_blocked,
                                    "the object was made after the version " + short_id(version) +
                                        " and cannot be removed: a disabled module needs it",
                                    *it);
        }
    }

    std::vector<ObjectId> touched = back;
    touched.insert(touched.end(), changed.begin(), changed.end());
    touched.insert(touched.end(), added.begin(), added.end());
    std::sort(touched.begin(), touched.end());
    command::Value::List result;
    for (const ObjectId id : touched) {
        result.emplace_back(id);
    }
    return command::Value(std::move(result));
}

// --- For ProjectSession --------------------------------------------------------------------------

void ProjectHistory::opened(const fs::path& folder) {
    const std::optional<std::string> text = io::read_file(journal_path(folder));
    pending_ = text ? journal_entries(*text) : std::vector<std::string>{};
}

void ProjectHistory::closed() {
    pending_.clear();
}

void ProjectHistory::saved(const fs::path& folder) {
    const fs::path file = journal_path(folder);
    std::error_code error;
    if (pending_.empty()) {
        fs::remove(file, error);
        return;
    }
    const std::string text = pending_journal();
    if (const std::optional<std::string> old = io::read_file(file); old && *old == text) {
        return;
    }
    fs::create_directories(file.parent_path(), error);
    if (error || !io::write_file(file, text)) {
        throw ProjectError(ProjectErrc::io, "cannot write the file", io::display(file));
    }
}

// --- Helpers -------------------------------------------------------------------------------------

const fs::path& ProjectHistory::folder() const {
    if (!session_.folder_) {
        throw ProjectError(ProjectErrc::no_folder,
                           "the project has no folder yet: save it to keep versions");
    }
    return *session_.folder_;
}

std::optional<Repository> ProjectHistory::repository() const {
    if (!session_.folder_) {
        return std::nullopt;
    }
    return Repository::open(*session_.folder_);
}

Repository ProjectHistory::repository_for_writing() const {
    return Repository::open_or_create(folder());
}

ProjectFiles ProjectHistory::current_files() const {
    if (session_.folder_) {
        return store_project(*session_.folder_, bus_.document(), session_.module_versions());
    }
    MemoryBlobStore blobs;
    return write_project(bus_.document(), session_.module_versions(), blobs);
}

bool ProjectHistory::differs(const Repository* repo, const ProjectFiles& files) const {
    if (const std::optional<std::string> head = repo ? repo->head().version : std::nullopt) {
        return !same_model(repo->read(*head).files, files);
    }
    MemoryBlobStore blobs;
    return !same_model(files, write_project(model::Document(), {}, blobs));
}

std::string ProjectHistory::pending_journal() const {
    std::string text = command::journal_header();
    for (const std::string& entry : pending_) {
        text += entry;
        text += '\n';
    }
    return text;
}

std::string ProjectHistory::make_version(Repository& repo, const ProjectFiles& files,
                                         std::string journal, const VersionNote& note) {
    const Head head = repo.head();
    std::vector<std::string> parents;
    if (head.version) {
        parents.push_back(*head.version);
    }
    const std::string id = repo.commit(files, journal, note, author_, parents);
    repo.set_branch(*head.branch, id, false);
    return id;
}

ProjectFiles ProjectHistory::load(const VersionFiles& version, std::string_view entry,
                                  std::optional<std::uint64_t> next_id) {
    const fs::path& project = folder();
    const FolderBlobStore blobs(objects_folder(project));
    LoadedProject loaded = read_project(version.files, blobs);
    const bool renumbered = next_id && *next_id > loaded.contents.next_id;
    if (renumbered) {
        loaded.contents.next_id = *next_id;
    }
    session_.replace(std::move(loaded), entry);
    ProjectFiles files = renumbered ? current_files() : version.files;
    (void)write_project_files(project, files);
    return files;
}

void ProjectHistory::forget_pending() {
    pending_.clear();
    if (session_.folder_) {
        std::error_code ignored;
        fs::remove(journal_path(*session_.folder_), ignored);
    }
}

void ProjectHistory::check_idle(std::string_view action) const {
    session_.check_idle(action);
}

}  // namespace fem::storage
