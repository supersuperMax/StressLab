#include "fem/core/user_folders.h"

#include "fem/core/product.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h must come first.
#include <knownfolders.h>
#include <shlobj.h>
#endif

namespace fem {

namespace {

std::filesystem::path folder_name(Platform platform) {
    std::string name(kProductName);
    if (platform == Platform::posix) {
        for (char& c : name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return std::filesystem::path(name);
}

const std::filesystem::path& require(const std::filesystem::path& path, const char* what) {
    if (path.empty()) {
        throw std::runtime_error(std::string("cannot find the folders of the user: ") + what +
                                 " is unknown");
    }
    return path;
}

/// An XDG variable if it is set to an absolute path (relative ones must be ignored), else the
/// default under the home folder. Absolute in the POSIX sense, so the rule can be checked on any
/// OS.
std::filesystem::path xdg(const std::filesystem::path& value, const std::filesystem::path& home,
                          const char* fallback) {
    if (!value.empty() && value.generic_u8string().front() == u8'/') {
        return value;
    }
    return require(home, "the home folder") / fallback;
}

#ifdef _WIN32
std::filesystem::path known_folder(const KNOWNFOLDERID& id) {
    PWSTR path = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) {
        result = std::filesystem::path(path);
    }
    CoTaskMemFree(path);
    return result;
}
#else
std::filesystem::path environment(const char* name) {
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read at startup.
    return value == nullptr ? std::filesystem::path() : std::filesystem::path(value);
}
#endif

}  // namespace

Platform current_platform() noexcept {
#if defined(_WIN32)
    return Platform::windows;
#elif defined(__APPLE__)
    return Platform::macos;
#else
    return Platform::posix;
#endif
}

UserFolders user_folders(Platform platform, const UserFolderInputs& inputs) {
    const std::filesystem::path name = folder_name(platform);
    switch (platform) {
        case Platform::windows:
            return {
                .config = require(inputs.roaming_appdata, "the Roaming AppData folder") / name,
                .logs = require(inputs.local_appdata, "the Local AppData folder") / name / "logs"};
        case Platform::macos: {
            const std::filesystem::path library =
                require(inputs.home, "the home folder") / "Library";
            return {.config = library / "Application Support" / name,
                    .logs = library / "Logs" / name};
        }
        case Platform::posix:
            return {
                .config = xdg(inputs.xdg_config_home, inputs.home, ".config") / name,
                .logs = xdg(inputs.xdg_state_home, inputs.home, ".local/state") / name / "logs"};
    }
    throw std::runtime_error("unknown platform");
}

UserFolderInputs user_folder_inputs() {
    UserFolderInputs inputs;
#ifdef _WIN32
    inputs.home = known_folder(FOLDERID_Profile);
    inputs.roaming_appdata = known_folder(FOLDERID_RoamingAppData);
    inputs.local_appdata = known_folder(FOLDERID_LocalAppData);
#else
    inputs.home = environment("HOME");
    inputs.xdg_config_home = environment("XDG_CONFIG_HOME");
    inputs.xdg_state_home = environment("XDG_STATE_HOME");
#endif
    return inputs;
}

UserFolders user_folders() {
    return user_folders(current_platform(), user_folder_inputs());
}

}  // namespace fem
