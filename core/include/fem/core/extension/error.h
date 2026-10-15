#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fem::extension {

/// What went wrong with a module: its manifest, its dependencies, its code or a profile.
enum class ExtensionErrc {
    invalid_manifest,       ///< The manifest is not valid; the error is a ManifestError.
    invalid_version,        ///< Text that is not a semantic version or a version range.
    unknown_extension,      ///< No module with this id is installed.
    incompatible,           ///< The module needs another version of the platform (`engine`).
    unresolved_dependency,  ///< A required module is missing, of another version or incompatible.
    dependency_cycle,       ///< Modules require each other.
    conflict,               ///< An id the module contributes is taken by another module.
    activation_failed,      ///< The code of the module could not be loaded or activated.
    command_failed,         ///< The code of the module reported that a command failed.
    invalid_state,          ///< Not possible now, e.g. disabling a module whose command is running.
    invalid_profile,        ///< A profile is not in the format the core writes.
    io,                     ///< A file cannot be read or written.
    unknown_setting,        ///< The code of a module asked for a setting that has no schema.
};

[[nodiscard]] std::string_view to_string(ExtensionErrc code) noexcept;

/// Error raised by the Extension Manager. The message is meant for developers of modules and for
/// logs; text shown to users is produced from `code()` through localization.
class ExtensionError : public std::runtime_error {
public:
    ExtensionError(ExtensionErrc code, const std::string& message);

    [[nodiscard]] ExtensionErrc code() const noexcept { return code_; }

private:
    ExtensionErrc code_;
};

/// A problem in a manifest, at a place in the file.
struct ManifestDiagnostic {
    std::uint32_t line = 0;    ///< 1-based.
    std::uint32_t column = 0;  ///< 1-based, in characters as toml++ counts them.
    std::string message;

    friend bool operator==(const ManifestDiagnostic&, const ManifestDiagnostic&) = default;
};

/// A manifest that cannot be used, with every problem found in it. `what()` has one line per
/// problem: "file:line:column: message".
class ManifestError : public ExtensionError {
public:
    /// Sorts the diagnostics by position; there must be at least one.
    ManifestError(std::filesystem::path file, std::vector<ManifestDiagnostic> diagnostics);

    /// The manifest file; empty for a manifest parsed from text.
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }
    /// The problems, by position in the file.
    [[nodiscard]] const std::vector<ManifestDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    std::filesystem::path file_;
    std::vector<ManifestDiagnostic> diagnostics_;
};

}  // namespace fem::extension
