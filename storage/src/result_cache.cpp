#include "fem/storage/result_cache.h"

#include "fem/core/command/python_literal.h"
#include "fem/core/io/files.h"
#include "fem/storage/blob_store.h"
#include "fem/storage/error.h"
#include "fem/storage/json.h"
#include "fem/storage/object_data_codec.h"
#include "fem/storage/repository.h"
#include "fem/storage/session.h"
#include "fem/storage/sha256.h"

#include "names.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <set>
#include <system_error>
#include <utility>

namespace fem::storage {

namespace {

namespace fs = std::filesystem;

/// The version of `entry.json` and of the records of pinned results.
constexpr std::int64_t record_format = 1;
/// The version of the text a key is the hash of; a new one changes every key.
constexpr std::int64_t key_format = 1;
constexpr std::string_view entry_file = "entry.json";
constexpr std::string_view files_folder = "files";
constexpr std::string_view objects_folder = ".fem/objects";
constexpr std::size_t copy_buffer_bytes = 1024 * 1024;

fs::path to_path(std::string_view utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

bool is_key(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::string hex(const Sha256::Digest& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : digest) {
        out += digits[byte >> 4];
        out += digits[byte & 0xF];
    }
    return out;
}

std::int64_t system_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// `stresslab.results.<function>(<arguments>)`.
std::string call(std::string_view function, std::string_view arguments = {}) {
    return "stresslab.results." + std::string(function) + "(" + std::string(arguments) + ")";
}

/// A name no other folder has, for a folder made whole before it gets its own name.
std::string unique_suffix() {
    static std::mt19937_64 engine{std::random_device{}()};
    constexpr std::string_view digits = "0123456789abcdef";
    std::uint64_t value = engine();
    std::string out;
    for (int i = 0; i < 12; ++i) {
        out += digits[value & 0xF];
        value >>= 4;
    }
    return out;
}

[[noreturn]] void throw_io(const std::string& message, const fs::path& path) {
    throw ProjectError(ProjectErrc::io, message, io::display(path));
}

void remove_folder(const fs::path& folder) {
    std::error_code error;
    fs::remove_all(folder, error);
    if (error) {
        throw_io("cannot remove the folder", folder);
    }
}

/// Removes a folder made half-way, keeping the error that stopped it.
void remove_quietly(const fs::path& folder) {
    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

// --- Records -------------------------------------------------------------------------------------

/// A result with, if pinned, the table of each file in the blob store.
struct Record {
    ResultInfo info;
    std::map<std::string, std::string> data;  ///< path -> table of its bytes
};

nlohmann::json to_json(const ResultInfo& info, const std::map<std::string, std::string>* data,
                       bool with_used) {
    nlohmann::json files = nlohmann::json::object();
    for (const ResultFile& file : info.files) {
        nlohmann::json value = {{"sha256", file.sha256}, {"size", file.size}};
        if (data != nullptr) {
            value["data"] = data->at(file.path);
        }
        files[file.path] = std::move(value);
    }
    nlohmann::json inputs = nlohmann::json::object();
    for (const auto& [name, value] : info.inputs) {
        inputs[name] = value;
    }
    nlohmann::json result = {{"created", info.created},
                             {"files", std::move(files)},
                             {"format", record_format},
                             {"inputs", std::move(inputs)},
                             {"key", info.key},
                             {"solver", {{"id", info.solver.id}, {"version", info.solver.version}}},
                             {"version", info.version}};
    if (with_used) {
        result["used"] = info.used;
    }
    return result;
}

[[noreturn]] void throw_invalid(const std::string& file, const std::string& message) {
    throw ProjectError(ProjectErrc::invalid_file, "a record of a result: " + message, file);
}

const nlohmann::json& member(const nlohmann::json& object, const char* name,
                             nlohmann::json::value_t type, const std::string& file) {
    const auto it = object.find(name);
    const bool fits =
        it != object.end() &&
        (it->type() == type || (type == nlohmann::json::value_t::number_integer &&
                                it->type() == nlohmann::json::value_t::number_unsigned));
    if (!fits) {
        throw_invalid(file, std::string("'") + name + "' is missing or of another type");
    }
    return *it;
}

/// A record read back. Throws ProjectError(invalid_file) with `file` if it is not one.
Record parse_record(std::string_view text, const std::string& file, bool pinned) {
    using Type = nlohmann::json::value_t;
    const nlohmann::json value = parse_json(text, file);
    if (!value.is_object()) {
        throw_invalid(file, "not an object");
    }
    if (member(value, "format", Type::number_integer, file).get<std::int64_t>() != record_format) {
        throw ProjectError(ProjectErrc::unsupported_format,
                           "a record of a result of another format", file);
    }
    Record record;
    ResultInfo& info = record.info;
    info.key = member(value, "key", Type::string, file).get<std::string>();
    info.version = member(value, "version", Type::string, file).get<std::string>();
    info.created = member(value, "created", Type::number_integer, file).get<std::int64_t>();
    info.used = pinned ? info.created
                       : member(value, "used", Type::number_integer, file).get<std::int64_t>();
    const nlohmann::json& solver = member(value, "solver", Type::object, file);
    info.solver.id = member(solver, "id", Type::string, file).get<std::string>();
    info.solver.version = member(solver, "version", Type::string, file).get<std::string>();
    for (const auto& [name, input] : member(value, "inputs", Type::object, file).items()) {
        if (!input.is_string()) {
            throw_invalid(file, "an input is not text");
        }
        info.inputs.emplace(name, input.get<std::string>());
    }
    if (!is_key(info.key)) {
        throw_invalid(file, "'" + info.key + "' is not a key");
    }
    for (const auto& [path, entry] : member(value, "files", Type::object, file).items()) {
        if (!detail::is_portable_relative_path(path)) {
            throw_invalid(file, "'" + path + "' is not a path inside the folder of the result");
        }
        ResultFile result_file;
        result_file.path = path;
        result_file.size = member(entry, "size", Type::number_integer, file).get<std::uint64_t>();
        result_file.sha256 = member(entry, "sha256", Type::string, file).get<std::string>();
        if (pinned) {
            std::string data = member(entry, "data", Type::string, file).get<std::string>();
            if (!is_blob_ref(data)) {
                throw_invalid(file, "'" + data + "' is not a block");
            }
            record.data.emplace(path, std::move(data));
        }
        info.bytes += result_file.size;
        info.files.push_back(std::move(result_file));
    }
    return record;
}

std::string pin_file(std::string_view key) {
    return "refs/results/" + std::string(key);
}

/// The result in the folder of a cache entry; none if the folder holds no readable entry (a
/// leftover of a crash, a file of someone else).
std::optional<ResultInfo> read_entry(const fs::path& folder) {
    const std::optional<std::string> text = io::read_file(folder / to_path(entry_file));
    if (!text) {
        return std::nullopt;
    }
    try {
        ResultInfo info =
            parse_record(*text, io::display(folder / to_path(entry_file)), false).info;
        if (info.key != io::display(folder.filename())) {
            return std::nullopt;
        }
        info.cached = true;
        info.folder = folder / to_path(files_folder);
        return info;
    } catch (const ProjectError&) {
        return std::nullopt;
    }
}

void write_entry(const fs::path& folder, const ResultInfo& info) {
    const fs::path file = folder / to_path(entry_file);
    if (!io::write_file(file, to_canonical_json(to_json(info, nullptr, true)))) {
        throw_io("cannot write the file", file);
    }
}

/// Makes `staged` (a folder made whole) the entry `target`, in place of one there.
void install(const fs::path& staged, const fs::path& target) {
    std::error_code error;
    if (fs::exists(target, error)) {
        // A folder cannot replace another on Windows: the old one is moved aside first.
        const fs::path old = target.parent_path() / to_path(".old-" + unique_suffix());
        fs::rename(target, old, error);
        if (error) {
            throw_io("cannot replace the result", target);
        }
        fs::rename(staged, target, error);
        if (error) {
            fs::rename(old, target, error);
            throw_io("cannot replace the result", target);
        }
        remove_quietly(old);
        return;
    }
    fs::rename(staged, target, error);
    if (error) {
        throw_io("cannot store the result", target);
    }
}

/// Copies a file, giving the SHA-256 of its bytes and their count.
ResultFile copy_hashed(const fs::path& from, const fs::path& to, std::string path) {
    std::ifstream in(from, std::ios::binary);
    if (!in) {
        throw_io("cannot read the file", from);
    }
    std::error_code error;
    fs::create_directories(to.parent_path(), error);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (error || !out) {
        throw_io("cannot write the file", to);
    }
    std::vector<char> buffer(copy_buffer_bytes);
    Sha256 sha;
    ResultFile file;
    file.path = std::move(path);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) {
            break;
        }
        sha.update(std::as_bytes(std::span(buffer.data(), got)));
        out.write(buffer.data(), static_cast<std::streamsize>(got));
        file.size += got;
    }
    if (in.bad()) {
        throw_io("cannot read the file", from);
    }
    out.close();
    if (!out) {
        throw_io("cannot write the file", to);
    }
    file.sha256 = hex(sha.finish());
    return file;
}

}  // namespace

// --- Keys ----------------------------------------------------------------------------------------

std::string result_key(const ProjectFiles& files, const SolverInfo& solver,
                       const ResultInputs& inputs) {
    // Checks the format of project.json and of refs/blobs.json.
    (void)read_blob_roots(files);
    const auto project = files.find(project_file);
    const nlohmann::json settings = parse_json(project->second, std::string(project_file));
    nlohmann::json blobs = nlohmann::json::object();
    if (const auto refs = files.find(blob_refs_file); refs != files.end()) {
        blobs = parse_json(refs->second, std::string(blob_refs_file));
    }
    nlohmann::json model = nlohmann::json::object();
    for (const auto& [path, bytes] : files) {
        if (path.starts_with(std::string(model_folder) + "/")) {
            model[path] = sha256_hex(std::as_bytes(std::span(bytes)));
        }
    }
    nlohmann::json named_inputs = nlohmann::json::object();
    for (const auto& [name, value] : inputs) {
        named_inputs[name] = value;
    }
    const nlohmann::json text = {
        {"blobs", std::move(blobs)},
        {"format", key_format},
        {"inputs", std::move(named_inputs)},
        {"model", std::move(model)},
        {"solver", {{"id", solver.id}, {"version", solver.version}}},
        {"units", settings.contains("units") ? settings.at("units") : nlohmann::json(nullptr)}};
    const std::string canonical = to_canonical_json(text);
    return sha256_hex(std::as_bytes(std::span(canonical)));
}

std::vector<std::string> pinned_result_roots(const Repository& repo) {
    std::set<std::string> roots;
    std::set<std::string> versions;
    for (const auto& [key, text] : repo.pins()) {
        Record record = parse_record(text, pin_file(key), true);
        for (auto& [path, data] : record.data) {
            roots.insert(std::move(data));
        }
        versions.insert(std::move(record.info.version));
    }
    for (const std::string& version : versions) {
        std::optional<std::string> project;
        std::optional<std::string> refs;
        try {
            project = repo.read_file(version, project_file);
            refs = repo.read_file(version, blob_refs_file);
        } catch (const ProjectError& error) {
            if (error.code() != ProjectErrc::unknown_version) {
                throw;
            }
        }
        if (!project || !refs) {
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

// --- ResultCache ---------------------------------------------------------------------------------

ResultCache::ResultCache(ProjectSession& session) : session_(session), clock_(system_now) {}

ResultCache::~ResultCache() = default;

void ResultCache::set_clock(Clock clock) {
    clock_ = clock ? std::move(clock) : Clock(system_now);
}

std::string ResultCache::key(const SolverInfo& solver, const ResultInputs& inputs) const {
    return result_key(session_.history().current_files(), solver, inputs);
}

std::string ResultCache::key_at(std::string_view version, const SolverInfo& solver,
                                const ResultInputs& inputs) const {
    const std::optional<Repository> repo =
        session_.folder_ ? Repository::open(*session_.folder_) : std::nullopt;
    if (!repo) {
        throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
    }
    return result_key(repo->read(repo->resolve(version)).files, solver, inputs);
}

fs::path ResultCache::cache_folder() const {
    if (!session_.folder_) {
        throw ProjectError(ProjectErrc::no_folder,
                           "the project has no folder yet: save it to keep results");
    }
    return *session_.folder_ / to_path(result_cache_folder);
}

ResultInfo ResultCache::put(std::string_view key, std::string_view version,
                            const SolverInfo& solver, const ResultInputs& inputs,
                            const fs::path& from) {
    const fs::path cache = cache_folder();
    const std::optional<Repository> repo = Repository::open(*session_.folder_);
    if (!repo) {
        throw ProjectError(ProjectErrc::unknown_version,
                           "the project has no versions yet: a result needs the version of the "
                           "model it was computed from");
    }
    const std::string id = repo->resolve(version);
    if (solver.id.empty()) {
        throw ProjectError(ProjectErrc::result_mismatch, "a result needs the id of its solver");
    }
    if (result_key(repo->read(id).files, solver, inputs) != key) {
        throw ProjectError(ProjectErrc::result_mismatch,
                           "'" + std::string(key) +
                               "' is not the key of the results of the version " + id +
                               " with this solver and these inputs");
    }
    std::error_code error;
    if (!fs::is_directory(from, error)) {
        throw_io("the result is not a folder", from);
    }

    fs::create_directories(cache, error);
    if (error) {
        throw_io("cannot create the folder", cache);
    }
    const fs::path staged = cache / to_path(".put-" + unique_suffix());
    ResultInfo info;
    info.key = std::string(key);
    info.version = id;
    info.solver = solver;
    info.inputs = inputs;
    info.created = clock_();
    info.used = info.created;
    try {
        const fs::path files = staged / to_path(files_folder);
        fs::create_directories(files, error);
        if (error) {
            throw_io("cannot create the folder", files);
        }
        std::vector<std::pair<std::string, fs::path>> sources;
        for (fs::recursive_directory_iterator it(from, error), end; !error && it != end;
             it.increment(error)) {
            if (it->is_regular_file(error)) {
                sources.emplace_back(io::display(it->path().lexically_relative(from)), it->path());
            }
        }
        if (error) {
            throw_io("cannot list the folder", from);
        }
        std::sort(sources.begin(), sources.end());
        for (const auto& [path, source] : sources) {
            ResultFile file = copy_hashed(source, files / to_path(path), path);
            info.bytes += file.size;
            info.files.push_back(std::move(file));
        }
        write_entry(staged, info);
        install(staged, cache / to_path(info.key));
    } catch (...) {
        remove_quietly(staged);
        throw;
    }
    evict(info.key);
    info.cached = true;
    info.pinned = repo->pin(info.key).has_value();
    info.folder = cache / to_path(info.key) / to_path(files_folder);
    return info;
}

std::optional<ResultInfo> ResultCache::find(std::string_view key) {
    if (!session_.folder_ || !is_key(key)) {
        return std::nullopt;
    }
    const fs::path cache = *session_.folder_ / to_path(result_cache_folder);
    const fs::path target = cache / to_path(key);
    const std::optional<Repository> repo = Repository::open(*session_.folder_);
    const std::optional<std::string> pinned = repo ? repo->pin(key) : std::nullopt;
    if (std::optional<ResultInfo> info = read_entry(target)) {
        info->used = clock_();
        write_entry(target, *info);
        info->pinned = pinned.has_value();
        return info;
    }
    if (!pinned) {
        return std::nullopt;
    }

    // Back from the history: the files from the blob store, checked against the record.
    const Record record = parse_record(*pinned, pin_file(key), true);
    ResultInfo info = record.info;
    info.used = clock_();
    const FolderBlobStore blobs(*session_.folder_ / to_path(objects_folder));
    std::error_code error;
    fs::create_directories(cache, error);
    if (error) {
        throw_io("cannot create the folder", cache);
    }
    const fs::path staged = cache / to_path(".find-" + unique_suffix());
    try {
        for (const ResultFile& file : info.files) {
            const fs::path path = staged / to_path(files_folder) / to_path(file.path);
            fs::create_directories(path.parent_path(), error);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (error || !out) {
                throw_io("cannot write the file", path);
            }
            Sha256 sha;
            const std::uint64_t size = get_byte_data(
                record.data.at(file.path), blobs, [&](std::span<const std::byte> bytes) {
                    sha.update(bytes);
                    out.write(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
                });
            out.close();
            if (!out) {
                throw_io("cannot write the file", path);
            }
            if (size != file.size || hex(sha.finish()) != file.sha256) {
                throw ProjectError(ProjectErrc::corrupt_blob,
                                   "the file " + file.path + " of the pinned result " +
                                       std::string(key) + " is not what its record says");
            }
        }
        // Empty folders of an empty result.
        fs::create_directories(staged / to_path(files_folder), error);
        write_entry(staged, info);
        if (fs::exists(target, error)) {
            remove_folder(target);  // An entry that cannot be read.
        }
        install(staged, target);
    } catch (...) {
        remove_quietly(staged);
        throw;
    }
    evict(info.key);
    info.cached = true;
    info.pinned = true;
    info.folder = target / to_path(files_folder);
    return info;
}

std::vector<ResultInfo> ResultCache::cached() const {
    std::vector<ResultInfo> result;
    if (!session_.folder_) {
        return result;
    }
    const fs::path cache = *session_.folder_ / to_path(result_cache_folder);
    std::error_code error;
    if (!fs::is_directory(cache, error)) {
        return result;
    }
    for (fs::directory_iterator it(cache, error), end; !error && it != end; it.increment(error)) {
        if (is_key(io::display(it->path().filename()))) {
            if (std::optional<ResultInfo> info = read_entry(it->path())) {
                result.push_back(std::move(*info));
            }
        }
    }
    if (error) {
        throw_io("cannot list the folder", cache);
    }
    return result;
}

std::vector<ResultInfo> ResultCache::list() const {
    std::map<std::string, ResultInfo> all;
    for (ResultInfo& info : cached()) {
        all.emplace(info.key, std::move(info));
    }
    const std::optional<Repository> repo =
        session_.folder_ ? Repository::open(*session_.folder_) : std::nullopt;
    if (repo) {
        for (const auto& [key, text] : repo->pins()) {
            if (const auto it = all.find(key); it != all.end()) {
                it->second.pinned = true;
                continue;
            }
            ResultInfo info = parse_record(text, pin_file(key), true).info;
            info.pinned = true;
            all.emplace(key, std::move(info));
        }
    }
    std::vector<ResultInfo> result;
    for (auto& [key, info] : all) {
        result.push_back(std::move(info));
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const ResultInfo& a, const ResultInfo& b) { return a.used > b.used; });
    return result;
}

void ResultCache::pin(std::string_view key, const Progress& progress) {
    check_idle("pinning a result");
    const fs::path cache = cache_folder();
    std::optional<Repository> repo = Repository::open(*session_.folder_);
    if (!repo) {
        throw ProjectError(ProjectErrc::unknown_version, "the project has no versions yet");
    }
    if (is_key(key) && repo->pin(key)) {
        session_.bus_.journal_action(call("pin", command::python_string(key)));
        return;
    }
    const fs::path target = cache / to_path(key);
    const std::optional<ResultInfo> info =
        is_key(key) ? read_entry(target) : std::optional<ResultInfo>();
    if (!info) {
        throw ProjectError(ProjectErrc::unknown_result,
                           "there is no result '" + std::string(key) + "' in the cache");
    }
    (void)repo->info(info->version);  // unknown_version if the version is gone

    FolderBlobStore blobs(*session_.folder_ / to_path(objects_folder));
    std::map<std::string, std::string> data;
    std::uint64_t done = 0;
    std::vector<char> buffer;
    for (const ResultFile& file : info->files) {
        const fs::path path = *info->folder / to_path(file.path);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw_io("cannot read the file", path);
        }
        Sha256 sha;
        std::uint64_t size = 0;
        const ByteSource read = [&](std::span<std::byte> out) -> std::size_t {
            progress.check();
            in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (in.bad()) {
                throw_io("cannot read the file", path);
            }
            sha.update(out.first(got));
            size += got;
            done += got;
            progress.step(info->bytes > 0
                              ? static_cast<double>(done) / static_cast<double>(info->bytes)
                              : 1.0);
            return got;
        };
        std::string table = put_byte_data(read, blobs);
        if (size != file.size || hex(sha.finish()) != file.sha256) {
            throw ProjectError(ProjectErrc::corrupt_blob, "the file " + file.path +
                                                              " of the result " + std::string(key) +
                                                              " changed in the cache");
        }
        data.emplace(file.path, std::move(table));
    }
    progress.check();
    repo->set_pin(key, to_canonical_json(to_json(*info, &data, false)));
    progress.step(1.0);
    session_.bus_.journal_action(call("pin", command::python_string(key)));
}

void ResultCache::unpin(std::string_view key) {
    check_idle("unpinning a result");
    (void)cache_folder();
    std::optional<Repository> repo = Repository::open(*session_.folder_);
    if (!repo || !is_key(key) || !repo->remove_pin(key)) {
        throw ProjectError(ProjectErrc::unknown_result,
                           "the result '" + std::string(key) + "' is not pinned");
    }
    session_.bus_.journal_action(call("unpin", command::python_string(key)));
}

bool ResultCache::remove(std::string_view key) {
    check_idle("removing a result");
    const fs::path cache = cache_folder();
    bool removed = false;
    if (is_key(key)) {
        const fs::path target = cache / to_path(key);
        std::error_code error;
        if (fs::exists(target, error)) {
            remove_folder(target);
            removed = true;
        }
    }
    session_.bus_.journal_action(call("remove", command::python_string(key)));
    return removed;
}

PruneStats ResultCache::prune_inactive() {
    check_idle("removing the results of inactive variants");
    const fs::path cache = cache_folder();
    std::set<std::string> active;
    if (const std::optional<Repository> repo = Repository::open(*session_.folder_)) {
        if (const std::optional<std::string> head = repo->head().version) {
            const std::vector<std::string> start{*head};
            for (const VersionInfo& version : repo->log(start)) {
                active.insert(version.id);
            }
        }
    }
    PruneStats stats;
    for (const ResultInfo& info : cached()) {
        if (!active.contains(info.version)) {
            remove_folder(cache / to_path(info.key));
            ++stats.removed;
            stats.freed_bytes += info.bytes;
        }
    }
    session_.bus_.journal_action(call("prune_inactive"));
    return stats;
}

std::uint64_t ResultCache::usage() const {
    std::uint64_t total = 0;
    for (const ResultInfo& info : cached()) {
        total += info.bytes;
    }
    return total;
}

void ResultCache::evict(std::string_view keep) {
    const fs::path cache = *session_.folder_ / to_path(result_cache_folder);
    // What crashes left: folders made half-way, entries that cannot be read.
    std::error_code error;
    std::vector<fs::path> leftovers;
    for (fs::directory_iterator it(cache, error), end; !error && it != end; it.increment(error)) {
        const std::string name = io::display(it->path().filename());
        if (name.starts_with(".") || (is_key(name) && !read_entry(it->path()))) {
            leftovers.push_back(it->path());
        }
    }
    for (const fs::path& leftover : leftovers) {
        remove_quietly(leftover);
    }
    if (limit_ == 0) {
        return;
    }
    std::vector<ResultInfo> entries = cached();
    std::uint64_t total = 0;
    for (const ResultInfo& info : entries) {
        total += info.bytes;
    }
    std::sort(entries.begin(), entries.end(), [](const ResultInfo& a, const ResultInfo& b) {
        return a.used != b.used ? a.used < b.used : a.key < b.key;
    });
    for (const ResultInfo& info : entries) {
        if (total <= limit_) {
            break;
        }
        if (info.key == keep) {
            continue;
        }
        remove_folder(cache / to_path(info.key));
        total -= info.bytes;
    }
}

void ResultCache::check_idle(std::string_view action) const {
    session_.check_idle(action);
}

}  // namespace fem::storage
