#pragma once

#include "fem/core/extension/semver.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
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

}  // namespace fem::extension
