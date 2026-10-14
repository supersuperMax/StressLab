#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fem::extension {

class ExtensionManager;

/// What the user changed in the module manager, kept between sessions (task 1.14): the modules
/// the user disabled and the folders the user installed modules from. Everything else is
/// installed and enabled as the application ships it. The file is JSON:
///
///     {
///       "disabled": ["fem.example"],
///       "format": "stresslab.modules",
///       "formatVersion": 1,
///       "local": ["D:/Мои модули/fem.mine"]
///     }
struct UserModules {
    /// Ids, sorted, each once.
    std::vector<std::string> disabled;
    /// Absolute folders, sorted, each once; written with '/'.
    std::vector<std::filesystem::path> local;

    friend bool operator==(const UserModules&, const UserModules&) = default;
};

/// Reads the state. Throws ExtensionError(invalid_profile) with the reason.
[[nodiscard]] UserModules parse_user_modules(std::string_view json);
/// The state as JSON, deterministically: keys sorted, two-space indent, UTF-8 as is, '\n' line
/// breaks and one at the end; parsing it and writing it again gives the same bytes. Throws
/// ExtensionError(invalid_profile) for a bad id or a folder that is not absolute.
[[nodiscard]] std::string to_json(const UserModules& modules);

/// Throws ExtensionError(io) if the file cannot be read, and as `parse_user_modules`.
[[nodiscard]] UserModules load_user_modules(const std::filesystem::path& file);
/// Creates the folder of the file if needed. Throws ExtensionError(io) if the file cannot be
/// written, and as `to_json`.
void save_user_modules(const std::filesystem::path& file, const UserModules& modules);

/// The state of `manager` now: the installed modules this platform can run that are disabled, and
/// the folders of the modules installed from a folder (`ExtensionSource::local`).
[[nodiscard]] UserModules user_modules(const ExtensionManager& manager);

}  // namespace fem::extension
