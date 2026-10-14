#pragma once

#include "fem/core/extension/semver.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fem::extension {

/// A module of a profile: its id and the version that was enabled when the profile was saved.
struct ProfileEntry {
    std::string id;
    Version version;

    friend bool operator==(const ProfileEntry&, const ProfileEntry&) = default;
};

/// A set of enabled modules with a window layout for a kind of work ("Strength", "Heat"),
/// switched in one click and passed to colleagues as a file (docs/architecture.md §4.5, §4.6
/// item 10). The file is JSON:
///
///     {
///       "extensions": [{"id": "fem.example", "version": "1.2.0"}],
///       "format": "stresslab.profile",
///       "formatVersion": 1,
///       "layout": {},
///       "name": "Strength"
///     }
///
/// `layout` belongs to the shell; the core keeps it as it is.
struct Profile {
    std::string name;
    /// Sorted by id, each id once.
    std::vector<ProfileEntry> extensions;
    nlohmann::json layout = nlohmann::json::object();

    friend bool operator==(const Profile&, const Profile&) = default;
};

/// Reads a profile. Throws ExtensionError(invalid_profile) with the reason.
[[nodiscard]] Profile parse_profile(std::string_view json);
/// The profile as JSON, deterministically: keys sorted, two-space indent, UTF-8 as is, '\n' line
/// breaks and one at the end; parsing it and writing it again gives the same bytes. Throws
/// ExtensionError(invalid_profile) for text that is not valid UTF-8 or a bad id.
[[nodiscard]] std::string to_json(const Profile& profile);

/// Throws ExtensionError(io) if the file cannot be read, and as `parse_profile`.
[[nodiscard]] Profile load_profile(const std::filesystem::path& file);
/// Throws ExtensionError(io) if the file cannot be written, and as `to_json`.
void save_profile(const std::filesystem::path& file, const Profile& profile);

/// The profiles of the user: one file each in a folder (`<settings folder>/profiles`), found by
/// the name inside the file. The file of a new profile is named after it (`file_name`), so the
/// folder is easy to look through and a file can be passed to a colleague as it is. Files that are
/// not profiles are skipped.
class ProfileLibrary {
public:
    /// The folder may not exist yet: `save` creates it.
    explicit ProfileLibrary(std::filesystem::path folder);

    [[nodiscard]] const std::filesystem::path& folder() const noexcept { return folder_; }
    /// Names of the profiles, sorted; a name in two files counts once (the first file by name).
    [[nodiscard]] std::vector<std::string> names() const;
    /// The profile of this name; nothing if there is none.
    [[nodiscard]] std::optional<Profile> find(std::string_view name) const;
    /// The file of the profile of this name; empty if there is none.
    [[nodiscard]] std::filesystem::path file_of(std::string_view name) const;
    /// Saves a profile, replacing the one of the same name, and returns its file. Throws
    /// ExtensionError(io) and as `to_json`.
    std::filesystem::path save(const Profile& profile);
    /// Deletes the profile of this name; false if there is none. Throws ExtensionError(io) if the
    /// file cannot be deleted.
    bool remove(std::string_view name);

    /// The file name for a profile name: the name as it is, with characters that file systems do
    /// not allow replaced by '_' and ".json" added ("Прочность стали.json").
    [[nodiscard]] static std::string file_name(std::string_view name);

private:
    /// Profiles in the folder with their files, by file name.
    [[nodiscard]] std::vector<std::pair<std::filesystem::path, Profile>> read_all() const;

    std::filesystem::path folder_;
};

}  // namespace fem::extension
