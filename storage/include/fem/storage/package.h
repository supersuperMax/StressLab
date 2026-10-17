#pragma once

#include "fem/storage/progress.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

// A project in one file to send to a colleague (docs/architecture.md §3.1,
// docs/adr/0021-result-cache-and-slpack.md): a `.slpack` is a zip of the project folder, with its
// history or without it. Any zip tool opens it.
namespace fem::storage {

/// The extension of packages.
inline constexpr std::string_view package_extension = ".slpack";

struct PackOptions {
    /// With the history: every version, variant, label, change put aside and pinned result, and
    /// the journal since the last version. Without it: the model as it is saved, alone.
    bool history = true;
};

/// What packing or unpacking did.
struct PackStats {
    std::size_t files = 0;             ///< Files in the package.
    std::uintmax_t bytes = 0;          ///< Their size unpacked.
    std::uintmax_t package_bytes = 0;  ///< The size of the package.
};

/// Packs the project of `folder` as it is saved into `package`, which is replaced whole or not at
/// all. The package holds project.json, model/, refs/blobs.json and the blocks of bulk data they
/// need; with the history also `.fem/git` (but its configuration, which belongs to the computer)
/// and `.fem/journal.slpy`, and the blocks of every version and pinned result. The cache of
/// results and blocks nothing refers to are never packed. The same folder gives the same bytes
/// on every computer: entries are sorted by path and dated 1980-01-01. Throws ProjectError:
/// not_a_project, unsupported_format, missing_blob or corrupt_blob for a block the project needs,
/// io, cancelled.
PackStats pack_project(const std::filesystem::path& folder, const std::filesystem::path& package,
                       const PackOptions& options = {}, const Progress& progress = {});

/// Unpacks `package` into `folder`, which must not exist or be empty and is made whole or not at
/// all. Every entry is checked before anything is written: a relative path of UTF-8 with '/', no
/// "." or "..", no ':' or '\\', no name Windows keeps for a device, nothing two file systems that
/// ignore case would make one file, only the files of a project (project.json, model/,
/// refs/blobs.json, .fem/objects/, .fem/git/, .fem/journal.slpy), no links and no encryption. The
/// history unpacked gets the configuration of this computer. Throws ProjectError: invalid_archive
/// for a file that is not such a package or is damaged, not_a_project if `folder` is a file or has
/// files, unsupported_format, io, cancelled.
PackStats unpack_project(const std::filesystem::path& package, const std::filesystem::path& folder,
                         const Progress& progress = {});

}  // namespace fem::storage
