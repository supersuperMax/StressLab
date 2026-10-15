#pragma once

#include "fem/core/model/document.h"
#include "fem/storage/project_format.h"

#include <cstddef>
#include <filesystem>
#include <string_view>

// A project folder on disk (docs/architecture.md §3.1): the files of `project_format.h` and the
// blocks of bulk data in `.fem/objects`.
namespace fem::storage {

/// The extension of project folders.
inline constexpr std::string_view project_extension = ".slproj";

/// True if `folder` has a project.json.
[[nodiscard]] bool is_project_folder(const std::filesystem::path& folder);

/// What a save did.
struct SaveStats {
    std::size_t written = 0;  ///< Files whose contents changed or that were new.
    std::size_t removed = 0;  ///< Files of objects that are gone.
};

/// Saves `document` into `folder`, creating it if needed. Writes only the files whose bytes
/// change, each one whole or not at all (a temporary file replaces it), project.json last, and
/// then removes the files of objects that are gone, so saving an unchanged model writes nothing
/// and the folder holds exactly the document. Throws ProjectError: not_a_project if `folder` is a
/// file or a folder with other files but no project.json (it is not ours to fill), io if
/// something cannot be written, and what `write_project` throws.
SaveStats save_project(const std::filesystem::path& folder, const model::Document& document,
                       const ModuleVersions& modules);

/// Reads the project of `folder` (`read_project`). Throws ProjectError: not_a_project without
/// project.json, io if a file cannot be read, and what `read_project` throws.
[[nodiscard]] LoadedProject load_project(const std::filesystem::path& folder);

}  // namespace fem::storage
