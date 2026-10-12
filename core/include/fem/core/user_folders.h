#pragma once

#include <cstdint>
#include <filesystem>

namespace fem {

/// The conventions an OS has for folders of an application's user data.
enum class Platform : std::uint8_t {
    windows,  ///< Known folders: Roaming and Local AppData.
    macos,    ///< ~/Library/Application Support and ~/Library/Logs.
    posix,    ///< The XDG Base Directory specification (Linux and other Unix systems).
};

/// The platform the core was built for.
[[nodiscard]] Platform current_platform() noexcept;

/// What the folders of the user are derived from; empty paths are unknown.
struct UserFolderInputs {
    std::filesystem::path home{};             ///< The home folder: HOME, or the profile on Windows.
    std::filesystem::path roaming_appdata{};  ///< Windows: FOLDERID_RoamingAppData.
    std::filesystem::path local_appdata{};    ///< Windows: FOLDERID_LocalAppData.
    std::filesystem::path xdg_config_home{};  ///< XDG_CONFIG_HOME; ignored unless absolute.
    std::filesystem::path xdg_state_home{};   ///< XDG_STATE_HOME; ignored unless absolute.
};

/// Where the application keeps files of the user. The folders may not exist yet.
struct UserFolders {
    /// Settings of the user (`settings.json`); roams with the user's profile on Windows.
    std::filesystem::path config;
    /// Log files; local to the machine.
    std::filesystem::path logs;
};

/// The folders by the conventions of `platform`, named after the product (in lower case on
/// posix). Throws std::runtime_error if an input they need is unknown.
///
///   windows: <roaming>/StressLab, <local>/StressLab/logs
///   macos:   ~/Library/Application Support/StressLab, ~/Library/Logs/StressLab
///   posix:   $XDG_CONFIG_HOME/stresslab (~/.config), $XDG_STATE_HOME/stresslab/logs
///            (~/.local/state)
[[nodiscard]] UserFolders user_folders(Platform platform, const UserFolderInputs& inputs);

/// The inputs of this process: known folders on Windows, environment variables elsewhere.
[[nodiscard]] UserFolderInputs user_folder_inputs();

/// The folders of the user running this process.
[[nodiscard]] UserFolders user_folders();

}  // namespace fem
