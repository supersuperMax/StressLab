#include "fem/storage/project.h"

#include "fem/core/io/files.h"
#include "fem/storage/blob_store.h"
#include "fem/storage/error.h"
#include "fem/storage/garbage.h"

#include "folders.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace fem::storage {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view objects_folder = ".fem/objects";

fs::path to_path(std::string_view utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

/// The path of `file` inside `folder` with '/', as a key of ProjectFiles.
std::string relative_key(const fs::path& file, const fs::path& folder) {
    return io::display(file.lexically_relative(folder));
}

[[noreturn]] void throw_io(const std::string& message, const fs::path& path) {
    throw ProjectError(ProjectErrc::io, message, io::display(path));
}

/// Files with this extension in `folder` and below, by their key; none if there is no folder.
std::vector<fs::path> files_below(const fs::path& folder, std::string_view extension) {
    std::vector<fs::path> result;
    std::error_code error;
    if (!fs::is_directory(folder, error)) {
        return result;
    }
    fs::recursive_directory_iterator it(folder, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == to_path(extension)) {
            result.push_back(it->path());
        }
    }
    if (error) {
        throw_io("cannot list the folder", folder);
    }
    return result;
}

void check_target(const fs::path& folder) {
    std::error_code error;
    if (!fs::exists(folder, error)) {
        return;
    }
    if (!fs::is_directory(folder, error)) {
        throw ProjectError(ProjectErrc::not_a_project, "a file is in the way of the project folder",
                           io::display(folder));
    }
    if (!is_project_folder(folder) && !fs::is_empty(folder, error)) {
        throw ProjectError(ProjectErrc::not_a_project,
                           "the folder has other files and is not a project", io::display(folder));
    }
}

void write_if_changed(const fs::path& file, const std::string& bytes, SaveStats& stats) {
    if (const std::optional<std::string> old = io::read_file(file); old && *old == bytes) {
        return;
    }
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    if (error || !io::write_file(file, bytes)) {
        throw_io("cannot write the file", file);
    }
    ++stats.written;
}

/// Writes `files` into `folder` and removes the files of objects that are gone.
SaveStats write_files(const fs::path& folder, const ProjectFiles& files);

}  // namespace

bool is_project_folder(const fs::path& folder) {
    std::error_code error;
    return fs::is_regular_file(folder / to_path(project_file), error);
}

SaveStats save_project(const fs::path& folder, const model::Document& document,
                       const ModuleVersions& modules) {
    // Blocks first, so that no file refers to a block that is not there yet.
    return write_files(folder, store_project(folder, document, modules));
}

ProjectFiles store_project(const fs::path& folder, const model::Document& document,
                           const ModuleVersions& modules) {
    check_target(folder);
    std::error_code error;
    const fs::path objects = folder / to_path(objects_folder);
    fs::create_directories(objects, error);
    if (error) {
        throw_io("cannot create the project folder", folder);
    }
    FolderBlobStore blobs(objects);
    return write_project(document, modules, blobs);
}

SaveStats write_project_files(const fs::path& folder, const ProjectFiles& files) {
    check_target(folder);
    return write_files(folder, files);
}

namespace {

SaveStats write_files(const fs::path& folder, const ProjectFiles& files) {
    std::error_code error;
    SaveStats stats;
    for (const auto& [key, bytes] : files) {
        if (key != project_file) {
            write_if_changed(folder / to_path(key), bytes, stats);
        }
    }
    write_if_changed(folder / to_path(project_file), files.at(std::string(project_file)), stats);

    // Files of removed objects, and temporary files a crash may have left. A file is kept if it is
    // the file just written, compared as files: where the file system ignores case, the folder of
    // type "Note" may be the folder "note" an older save made.
    std::map<fs::path, fs::path> written;  // "<id>.json" -> where it was written
    for (const auto& [key, bytes] : files) {
        const fs::path file = folder / to_path(key);
        written.emplace(file.filename(), file);
    }
    const fs::path model = folder / to_path(model_folder);
    std::vector<fs::path> stale = files_below(model, ".tmp");
    for (const fs::path& file : files_below(model, ".json")) {
        const auto it = written.find(file.filename());
        if (it == written.end() || !fs::equivalent(file, it->second, error)) {
            stale.push_back(file);
        }
    }
    for (const fs::path& file : stale) {
        if (!fs::remove(file, error) || error) {
            throw_io("cannot remove the file", file);
        }
        ++stats.removed;
    }
    detail::remove_empty_folders(model);
    return stats;
}

}  // namespace

CleanStats clean_project(const fs::path& folder, std::span<const std::string> extra_roots) {
    if (!is_project_folder(folder)) {
        throw ProjectError(ProjectErrc::not_a_project, "the folder has no project.json",
                           io::display(folder));
    }
    ProjectFiles files;
    for (const std::string_view key : {project_file, blob_refs_file}) {
        const fs::path file = folder / to_path(key);
        std::optional<std::string> bytes = io::read_file(file);
        if (!bytes) {
            // No refs/blobs.json is not "no blocks": without it nothing is known to be garbage.
            throw_io("cannot read the file", file);
        }
        files.emplace(std::string(key), std::move(*bytes));
    }
    std::vector<std::string> roots = read_blob_roots(files);
    roots.insert(roots.end(), extra_roots.begin(), extra_roots.end());
    FolderBlobStore blobs(folder / to_path(objects_folder));
    return collect_garbage(blobs, roots);
}

LoadedProject load_project(const fs::path& folder) {
    const ProjectFiles files = read_project_files(folder);
    const FolderBlobStore blobs(folder / to_path(objects_folder));
    return read_project(files, blobs);
}

ProjectFiles read_project_files(const fs::path& folder) {
    if (!is_project_folder(folder)) {
        throw ProjectError(ProjectErrc::not_a_project, "the folder has no project.json",
                           io::display(folder));
    }
    ProjectFiles files;
    const auto read = [&files, &folder](const fs::path& file) {
        std::optional<std::string> bytes = io::read_file(file);
        if (!bytes) {
            throw_io("cannot read the file", file);
        }
        files.emplace(relative_key(file, folder), std::move(*bytes));
    };
    read(folder / to_path(project_file));
    if (const fs::path refs = folder / to_path(blob_refs_file); fs::exists(refs)) {
        read(refs);
    }
    for (const fs::path& file : files_below(folder / to_path(model_folder), ".json")) {
        read(file);
    }
    return files;
}

}  // namespace fem::storage
