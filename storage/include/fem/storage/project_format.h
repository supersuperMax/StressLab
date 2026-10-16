#pragma once

#include "fem/core/model/contents.h"
#include "fem/core/model/document.h"
#include "fem/core/model/object_id.h"
#include "fem/storage/blob_store.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The files of a project folder (docs/architecture.md §3.1, docs/adr/0017-project-folder.md),
// made from a document and read back into one, in memory: the folder itself is `project.h`.
//
//     project.json                      format, modules with versions, next object id, units
//     model/<type>/<id>.json            an object of the core (mesh, node_set...)
//     model/ext/<owner>/<type>/<id>.json  an object of a module, enabled or not
//     refs/blobs.json                   {"<object id>": "sha256:..."}: blocks of bulk data
//
// Every file is canonical JSON (`json.h`), so writing what was read gives the same bytes.
namespace fem::storage {

/// The version of the layout and of the files; a project of a later version is not opened.
/// 2 since task 2.2: bulk data in pieces with zstd (docs/adr/0018-blob-store.md).
inline constexpr std::int64_t project_format_version = 2;

inline constexpr std::string_view project_file = "project.json";
inline constexpr std::string_view blob_refs_file = "refs/blobs.json";
inline constexpr std::string_view model_folder = "model";

/// Modules a project needs and their versions: id -> "major.minor.patch", "" if unknown.
using ModuleVersions = std::map<std::string, std::string, std::less<>>;

/// Files of a project: path relative to its folder with '/' -> contents.
using ProjectFiles = std::map<std::string, std::string, std::less<>>;

/// A project as read from its files.
struct LoadedProject {
    model::DocumentContents contents;
    ModuleVersions modules;
    /// The file of each object, for errors about it.
    std::map<model::ObjectId, std::string> files;
};

/// The file of an object of this owner and type. Characters other than ASCII letters, digits,
/// '.', '_' and '-' are written as %XX, and so are names that a file system refuses ("aux",
/// "con", a trailing dot), so any owner and type make a portable path.
[[nodiscard]] std::string object_file(std::string_view owner, std::string_view type,
                                      model::ObjectId id);

/// The files of `document`; the bulk data of its objects goes to `blobs`. `modules` gives the
/// versions of the owners of its objects; an owner missing from it is written with "".
/// Throws ProjectError(invalid_data) for what cannot be written (a non-finite number) and
/// whatever `blobs` throws.
[[nodiscard]] ProjectFiles write_project(const model::Document& document,
                                         const ModuleVersions& modules, BlobStore& blobs);

/// A project from its files: `project.json`, `refs/blobs.json` and every `.json` under `model/`
/// (others are ignored). Checks the files but not the model they make: that is the document's job
/// (`CommandBus::replace_document`). Throws ProjectError: not_a_project without `project.json`,
/// unsupported_format for a later format, invalid_file with the file and line, and what `blobs`
/// throws.
[[nodiscard]] LoadedProject read_project(const ProjectFiles& files, const BlobStore& blobs);

/// The blocks the files refer to (the values of `refs/blobs.json`), sorted, each once: the roots
/// of the garbage collection. Reads `project.json` first, so that a project of another format,
/// whose blocks may be laid out otherwise, is not cleaned. Throws as `read_project` does.
[[nodiscard]] std::vector<std::string> read_blob_roots(const ProjectFiles& files);

/// An object as its file describes it, without decoding its bulk data.
struct ObjectRecord {
    /// The object; `init.data` is null.
    model::StoredObject object;
    /// What the file says about the data (`describe_object_data`); null without data.
    nlohmann::json data;
    /// The block of the data in refs/blobs.json; empty without data.
    std::string blob;
    /// The file of the object.
    std::string file;
};

/// A project as its files describe it, with no block read: what a diff of versions compares
/// (task 2.4).
struct ProjectRecords {
    model::UnitSystem units{};
    ModuleVersions modules;
    std::uint64_t next_id = 1;
    std::map<model::ObjectId, ObjectRecord> objects;
};

/// The records of the files, checked as `read_project` checks them except that the data is not
/// decoded (so a data description is not compared with its block). Throws as `read_project` does.
[[nodiscard]] ProjectRecords read_project_records(const ProjectFiles& files);

/// One object of the files, found by its id, with its bulk data; none if the files do not have it.
/// Only that object is read and only its data is decoded (restoring one object from an older
/// version, task 2.3). Throws as `read_project` does about the files it reads.
[[nodiscard]] std::optional<model::StoredObject> read_object(const ProjectFiles& files,
                                                             const BlobStore& blobs,
                                                             model::ObjectId id);

}  // namespace fem::storage
