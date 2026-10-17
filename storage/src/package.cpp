#include "fem/storage/package.h"

#include "fem/core/io/files.h"
#include "fem/storage/blob_store.h"
#include "fem/storage/error.h"
#include "fem/storage/garbage.h"
#include "fem/storage/history.h"
#include "fem/storage/project.h"
#include "fem/storage/project_format.h"
#include "fem/storage/repository.h"

#include "names.h"
#include <miniz.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fem::storage {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view objects_folder = ".fem/objects";
constexpr std::string_view git_folder = ".fem/git";
constexpr std::string_view git_objects_folder = ".fem/git/objects";
/// Files of a history that belong to the computer, not to the project: never packed, and refused
/// in a package (an alternate makes git read objects from another folder of the computer).
constexpr std::string_view git_config_file = ".fem/git/config";
constexpr std::string_view git_alternates_file = ".fem/git/objects/info/alternates";
/// Archive names longer than this are refused (miniz keeps names of 512 bytes).
constexpr std::size_t max_name_bytes = 400;

fs::path to_path(std::string_view utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

[[noreturn]] void throw_io(const std::string& message, const fs::path& path) {
    throw ProjectError(ProjectErrc::io, message, io::display(path));
}

[[noreturn]] void throw_invalid(const std::string& message, const fs::path& package) {
    throw ProjectError(ProjectErrc::invalid_archive, message, io::display(package));
}

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

/// A file or folder made beside `target` to become it whole: `.<name>.<what>-<random>`.
fs::path staging_path(const fs::path& target, std::string_view what) {
    fs::path name = to_path(".");
    name += target.filename();
    name += to_path("." + std::string(what) + "-" + unique_suffix());
    return target.parent_path() / name;
}

void remove_quietly(const fs::path& path) {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

/// The time of every entry: 1980-01-01 00:00, the first a zip can hold. miniz turns a time into
/// the date of a zip by the local time zone, so the local midnight is given, and every computer
/// writes the same bytes.
MZ_TIME_T entry_time() {
    std::tm date{};
    date.tm_year = 80;
    date.tm_mon = 0;
    date.tm_mday = 1;
    date.tm_isdst = 0;
    return std::mktime(&date);
}

std::string zip_error(mz_zip_archive& zip) {
    return mz_zip_get_error_string(mz_zip_get_last_error(&zip));
}

// --- Packing -------------------------------------------------------------------------------------

/// An entry of a package: a file in memory, a file of the folder, or an empty folder.
struct Entry {
    std::string name;  ///< In the archive; a folder ends with '/'.
    std::optional<std::string> bytes{};
    fs::path file{};
    std::uint64_t size = 0;
    bool compress = true;
};

/// Files already compressed by zstd or zlib are stored as they are.
bool is_compressed(std::string_view name) {
    return name.starts_with(std::string(objects_folder) + "/") ||
           name.starts_with(std::string(git_objects_folder) + "/");
}

std::string relative_name(const fs::path& file, const fs::path& folder) {
    return io::display(file.lexically_relative(folder));
}

/// The files and empty folders of `.fem/git`, but its configuration.
void add_history(const fs::path& folder, std::vector<Entry>& entries) {
    const fs::path git = folder / to_path(git_folder);
    std::error_code error;
    for (fs::recursive_directory_iterator it(git, error), end; !error && it != end;
         it.increment(error)) {
        const std::string name = relative_name(it->path(), folder);
        if (it->is_directory(error)) {
            if (fs::is_empty(it->path(), error)) {
                entries.push_back({.name = name + "/", .compress = false});
            }
            continue;
        }
        if (!it->is_regular_file(error) || name == git_config_file) {
            continue;
        }
        const std::uintmax_t size = it->file_size(error);
        entries.push_back(
            {.name = name, .file = it->path(), .size = size, .compress = !is_compressed(name)});
    }
    if (error) {
        throw_io("cannot list the folder", git);
    }
}

std::vector<Entry> package_entries(const fs::path& folder, const PackOptions& options) {
    if (!is_project_folder(folder)) {
        throw ProjectError(ProjectErrc::not_a_project, "the folder has no project.json",
                           io::display(folder));
    }
    ProjectFiles files = read_project_files(folder);
    std::vector<std::string> roots = read_blob_roots(files);  // Checks the format too.
    std::optional<Repository> repo;
    if (options.history) {
        repo = Repository::open(folder);
        if (repo) {
            for (std::string& root : history_blob_roots(*repo)) {
                roots.push_back(std::move(root));
            }
        }
    }

    std::vector<Entry> entries;
    for (auto& [name, bytes] : files) {
        const std::size_t size = bytes.size();
        entries.push_back({.name = name, .bytes = std::move(bytes), .size = size});
    }
    const FolderBlobStore blobs(folder / to_path(objects_folder));
    for (const std::string& ref : reachable_blobs(blobs, roots)) {
        const fs::path file = blobs.file_of(ref);
        std::error_code error;
        const std::uintmax_t size = fs::file_size(file, error);
        if (error) {
            throw ProjectError(ProjectErrc::missing_blob,
                               "the block " + ref + " the project needs is not in it",
                               io::display(file));
        }
        entries.push_back(
            {.name = relative_name(file, folder), .file = file, .size = size, .compress = false});
    }
    if (repo) {
        add_history(folder, entries);
        const fs::path journal = folder / to_path(version_journal_file);
        std::error_code error;
        if (fs::is_regular_file(journal, error)) {
            entries.push_back({.name = std::string(version_journal_file),
                               .file = journal,
                               .size = fs::file_size(journal, error)});
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return entries;
}

struct Output {
    std::ofstream stream;
};

std::size_t write_at(void* opaque, mz_uint64 offset, const void* buffer, std::size_t size) {
    auto& out = static_cast<Output*>(opaque)->stream;
    out.seekp(static_cast<std::streamoff>(offset));
    out.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
    return out ? size : 0;
}

/// A file of the folder read into an entry, with the progress of the whole package.
struct Input {
    std::ifstream stream;
    const Progress* progress = nullptr;
    std::uintmax_t* done = nullptr;
    std::uintmax_t total = 0;
    bool cancelled = false;
};

std::size_t read_at(void* opaque, mz_uint64 offset, void* buffer, std::size_t size) {
    auto& in = *static_cast<Input*>(opaque);
    if (in.progress != nullptr && in.progress->cancelled && in.progress->cancelled()) {
        in.cancelled = true;
        return 0;
    }
    in.stream.clear();
    in.stream.seekg(static_cast<std::streamoff>(offset));
    in.stream.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    const auto got = static_cast<std::size_t>(in.stream.gcount());
    if (in.done != nullptr) {
        *in.done += got;
        in.progress->step(
            in.total > 0 ? static_cast<double>(*in.done) / static_cast<double>(in.total) : 1.0);
    }
    return got;
}

/// Ends the writing of an archive however it went.
struct Writer {
    mz_zip_archive zip{};
    bool open = false;
    ~Writer() {
        if (open) {
            mz_zip_writer_end(&zip);
        }
    }
};

/// Ends the reading of an archive however it went.
struct Reader {
    mz_zip_archive zip{};
    bool open = false;
    ~Reader() {
        if (open) {
            mz_zip_reader_end(&zip);
        }
    }
};

void write_package(const std::vector<Entry>& entries, const fs::path& file,
                   const Progress& progress, PackStats& stats) {
    Output output;
    output.stream.open(file, std::ios::binary | std::ios::trunc);
    if (!output.stream) {
        throw_io("cannot write the file", file);
    }
    Writer writer;
    writer.zip.m_pWrite = write_at;
    writer.zip.m_pIO_opaque = &output;
    if (!mz_zip_writer_init_v2(&writer.zip, 0, 0)) {
        throw_io("cannot start the package: " + zip_error(writer.zip), file);
    }
    writer.open = true;

    MZ_TIME_T time = entry_time();
    std::uintmax_t total = 0;
    for (const Entry& entry : entries) {
        total += entry.size;
    }
    std::uintmax_t done = 0;
    for (const Entry& entry : entries) {
        progress.check();
        const mz_uint level = entry.compress ? MZ_DEFAULT_LEVEL : MZ_NO_COMPRESSION;
        bool added = false;
        bool cancelled = false;
        if (entry.bytes || entry.name.ends_with('/')) {
            const std::string empty;
            const std::string& bytes = entry.bytes ? *entry.bytes : empty;
            added = mz_zip_writer_add_mem_ex_v2(&writer.zip, entry.name.c_str(), bytes.data(),
                                                bytes.size(), nullptr, 0, level, 0, 0, &time,
                                                nullptr, 0, nullptr, 0) != 0;
            done += bytes.size();
            progress.step(total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 1.0);
        } else {
            Input input;
            input.stream.open(entry.file, std::ios::binary);
            if (!input.stream) {
                throw_io("cannot read the file", entry.file);
            }
            input.progress = &progress;
            input.done = &done;
            input.total = total;
            added = mz_zip_writer_add_read_buf_callback(&writer.zip, entry.name.c_str(), read_at,
                                                        &input, entry.size, &time, nullptr, 0,
                                                        level, nullptr, 0, nullptr, 0) != 0;
            cancelled = input.cancelled;
        }
        if (cancelled) {
            progress.check();
            throw ProjectError(ProjectErrc::cancelled, "packing was cancelled");
        }
        if (!added) {
            throw_io("cannot add " + entry.name + " to the package: " + zip_error(writer.zip),
                     entry.file.empty() ? file : entry.file);
        }
        if (!entry.name.ends_with('/')) {
            ++stats.files;
            stats.bytes += entry.size;
        }
    }
    if (!mz_zip_writer_finalize_archive(&writer.zip)) {
        throw_io("cannot finish the package: " + zip_error(writer.zip), file);
    }
    writer.open = false;
    mz_zip_writer_end(&writer.zip);
    output.stream.close();
    if (!output.stream) {
        throw_io("cannot write the file", file);
    }
    std::error_code error;
    stats.package_bytes = fs::file_size(file, error);
}

// --- Unpacking -----------------------------------------------------------------------------------

struct ArchiveInput {
    std::ifstream stream;
};

std::size_t read_archive(void* opaque, mz_uint64 offset, void* buffer, std::size_t size) {
    auto& in = static_cast<ArchiveInput*>(opaque)->stream;
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(in.gcount());
}

struct FileOutput {
    std::ofstream stream;
    const Progress* progress = nullptr;
    std::uintmax_t* done = nullptr;
    std::uintmax_t total = 0;
    bool cancelled = false;
};

std::size_t write_extracted(void* opaque, mz_uint64 /*offset*/, const void* buffer,
                            std::size_t size) {
    auto& out = *static_cast<FileOutput*>(opaque);
    if (out.progress->cancelled && out.progress->cancelled()) {
        out.cancelled = true;
        return 0;
    }
    out.stream.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
    *out.done += size;
    out.progress->step(
        out.total > 0 ? static_cast<double>(*out.done) / static_cast<double>(out.total) : 1.0);
    return out.stream ? size : 0;
}

/// What a package may hold: the files of a project and nothing else.
bool is_allowed(std::string_view name, bool folder) {
    if (name == git_config_file || name == git_alternates_file) {
        return false;
    }
    if (!folder &&
        (name == project_file || name == blob_refs_file || name == version_journal_file)) {
        return true;
    }
    for (const std::string_view prefix :
         {std::string_view("model/"), std::string_view(".fem/objects/"),
          std::string_view(".fem/git/")}) {
        if (name.starts_with(prefix)) {
            return true;
        }
    }
    return folder && (name == model_folder || name == "refs" || name == ".fem" ||
                      name == objects_folder || name == git_folder);
}

/// An entry of a package, checked.
struct Member {
    mz_uint index = 0;
    std::string name;  ///< Without the '/' of a folder.
    bool folder = false;
    std::uint64_t size = 0;
};

std::vector<Member> check_members(mz_zip_archive& zip, const fs::path& package) {
    std::vector<Member> members;
    std::set<std::string> files;    // folded
    std::set<std::string> folders;  // folded
    bool has_project = false;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            throw_invalid("the package is damaged: " + zip_error(zip), package);
        }
        const mz_uint length = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
        if (length == 0 || length > max_name_bytes) {
            throw_invalid("an entry of the package has no name or too long a name", package);
        }
        std::string name(length, '\0');
        (void)mz_zip_reader_get_filename(&zip, i, name.data(), length);
        name.resize(length - 1);
        const bool folder = name.ends_with('/');
        if (folder) {
            name.pop_back();
        }
        if (stat.m_is_encrypted || !stat.m_is_supported) {
            throw_invalid("the entry '" + name + "' is encrypted or compressed in an unknown way",
                          package);
        }
        // A symbolic link made on a Unix: the type bits of its mode in the high half.
        constexpr std::uint32_t type_mask = 0170000;
        constexpr std::uint32_t link_type = 0120000;
        if ((stat.m_version_made_by >> 8) == 3 &&
            ((stat.m_external_attr >> 16) & type_mask) == link_type) {
            throw_invalid("the entry '" + name + "' is a link", package);
        }
        const bool ascii = std::all_of(name.begin(), name.end(),
                                       [](char c) { return static_cast<unsigned char>(c) < 0x80; });
        if (!ascii && (stat.m_bit_flag & (1U << 11)) == 0) {
            throw_invalid("the name of an entry is not marked as UTF-8", package);
        }
        if (!detail::is_portable_relative_path(name) || !is_allowed(name, folder)) {
            throw_invalid("'" + name + "' is not a file of a project", package);
        }
        const std::string folded = detail::fold_case(name);
        if (folder ? !folders.insert(folded).second
                   : (!files.insert(folded).second || folders.contains(folded))) {
            throw_invalid("'" + name + "' is in the package twice (letters in another case count)",
                          package);
        }
        has_project = has_project || (!folder && name == project_file);
        members.push_back(
            {.index = i, .name = std::move(name), .folder = folder, .size = stat.m_uncomp_size});
    }
    // A file must not be where a folder of another entry is ("model" and "model/a.json").
    for (const Member& member : members) {
        const std::string folded = detail::fold_case(member.name);
        for (std::size_t slash = folded.find('/'); slash != std::string::npos;
             slash = folded.find('/', slash + 1)) {
            if (files.contains(folded.substr(0, slash))) {
                throw_invalid("'" + member.name + "' is inside a file of the package", package);
            }
        }
        if (member.folder && files.contains(folded)) {
            throw_invalid("'" + member.name + "' is in the package twice", package);
        }
    }
    if (!has_project) {
        throw_invalid("the package has no project.json: it is not a project", package);
    }
    return members;
}

void check_target(const fs::path& folder) {
    std::error_code error;
    if (!fs::exists(folder, error)) {
        return;
    }
    if (!fs::is_directory(folder, error) || !fs::is_empty(folder, error)) {
        throw ProjectError(ProjectErrc::not_a_project,
                           "a package is unpacked only into a new or an empty folder",
                           io::display(folder));
    }
}

}  // namespace

PackStats pack_project(const fs::path& folder, const fs::path& package, const PackOptions& options,
                       const Progress& progress) {
    const std::vector<Entry> entries = package_entries(folder, options);
    std::error_code error;
    if (!package.parent_path().empty()) {
        fs::create_directories(package.parent_path(), error);
    }
    const fs::path staged = staging_path(package, "pack");
    PackStats stats;
    try {
        write_package(entries, staged, progress, stats);
        progress.check();
        fs::rename(staged, package, error);
        if (error) {
            throw_io("cannot write the file", package);
        }
    } catch (...) {
        remove_quietly(staged);
        throw;
    }
    progress.step(1.0);
    return stats;
}

PackStats unpack_project(const fs::path& package, const fs::path& folder,
                         const Progress& progress) {
    ArchiveInput input;
    input.stream.open(package, std::ios::binary);
    std::error_code error;
    const std::uintmax_t size = fs::file_size(package, error);
    if (!input.stream || error) {
        throw_io("cannot read the file", package);
    }
    Reader reader;
    reader.zip.m_pRead = read_archive;
    reader.zip.m_pIO_opaque = &input;
    if (!mz_zip_reader_init(&reader.zip, size, 0)) {
        throw_invalid("the file is not a zip: " + zip_error(reader.zip), package);
    }
    reader.open = true;
    const std::vector<Member> members = check_members(reader.zip, package);
    check_target(folder);

    PackStats stats;
    stats.package_bytes = size;
    std::uintmax_t total = 0;
    for (const Member& member : members) {
        total += member.size;
    }
    std::uintmax_t done = 0;
    if (!folder.parent_path().empty()) {
        fs::create_directories(folder.parent_path(), error);
    }
    const fs::path staged = staging_path(folder, "unpack");
    try {
        fs::create_directories(staged, error);
        if (error) {
            throw_io("cannot create the folder", staged);
        }
        for (const Member& member : members) {
            progress.check();
            const fs::path path = staged / to_path(member.name);
            if (member.folder) {
                fs::create_directories(path, error);
                if (error) {
                    throw_io("cannot create the folder", path);
                }
                continue;
            }
            fs::create_directories(path.parent_path(), error);
            FileOutput output;
            output.stream.open(path, std::ios::binary | std::ios::trunc);
            if (error || !output.stream) {
                throw_io("cannot write the file", path);
            }
            output.progress = &progress;
            output.done = &done;
            output.total = total;
            const bool extracted = mz_zip_reader_extract_to_callback(
                                       &reader.zip, member.index, write_extracted, &output, 0) != 0;
            if (output.cancelled) {
                throw ProjectError(ProjectErrc::cancelled, "unpacking was cancelled");
            }
            output.stream.close();
            if (!output.stream) {
                throw_io("cannot write the file", path);
            }
            if (!extracted) {
                throw_invalid(
                    "the entry '" + member.name + "' is damaged: " + zip_error(reader.zip),
                    package);
            }
            ++stats.files;
            stats.bytes += member.size;
        }
        // A project of a format this version reads, with a history that opens.
        (void)read_blob_roots(read_project_files(staged));
        if (fs::is_directory(staged / to_path(git_folder), error)) {
            try {
                Repository::adopt(staged);
                if (const std::optional<Repository> repo = Repository::open(staged)) {
                    (void)repo->head();
                }
            } catch (const ProjectError& history_error) {
                throw_invalid(std::string("the history of the package does not open: ") +
                                  history_error.what(),
                              package);
            }
        }
        progress.check();
        if (fs::exists(folder, error)) {
            fs::remove(folder, error);  // An empty folder.
        }
        fs::rename(staged, folder, error);
        if (error) {
            throw_io("cannot create the project folder", folder);
        }
    } catch (...) {
        remove_quietly(staged);
        throw;
    }
    progress.step(1.0);
    return stats;
}

}  // namespace fem::storage
