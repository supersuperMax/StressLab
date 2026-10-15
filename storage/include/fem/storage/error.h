#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fem::storage {

/// What went wrong with a project folder. Text for users is built from the code through
/// localization ("project.error.<code>" of the shell, with the file and line); `what()` is for
/// logs and tests.
enum class ProjectErrc {
    io,                  ///< A file or folder cannot be read or written.
    not_a_project,       ///< No project.json, or a folder with other files where one is created.
    unsupported_format,  ///< The project was saved by a newer version of the format.
    invalid_file,        ///< A file of the project is not what the format says.
    invalid_model,       ///< The files are well-formed, but the model they describe is not valid.
    missing_blob,        ///< A block of bulk data the model refers to is not in the project.
    corrupt_blob,        ///< A block does not match its hash or cannot be decoded.
    invalid_data,        ///< Data that cannot be saved (a NaN in properties, an unknown data type).
    no_folder,           ///< "Save" for a project that has no folder yet: "Save as" is needed.
};

[[nodiscard]] std::string_view to_string(ProjectErrc code) noexcept;

class ProjectError : public std::runtime_error {
public:
    /// `file`: the file of the project the error is about, relative to its folder with '/'
    /// ("model/mesh/3.json"), or a path outside it; empty if none. `line`: 1-based, 0 if unknown.
    ProjectError(ProjectErrc code, const std::string& message, std::string file = {}, int line = 0);

    [[nodiscard]] ProjectErrc code() const noexcept { return code_; }
    [[nodiscard]] const std::string& file() const noexcept { return file_; }
    [[nodiscard]] int line() const noexcept { return line_; }

private:
    ProjectErrc code_;
    std::string file_;
    int line_;
};

}  // namespace fem::storage
