#include "fem/core/extension/profile.h"

#include "fem/core/extension/error.h"
#include "fem/core/extension/manifest.h"

#include "files.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace fem::extension {

namespace {

constexpr std::string_view format_name = "stresslab.profile";
constexpr std::int64_t format_version = 1;

[[noreturn]] void fail(const std::string& reason) {
    throw ExtensionError(ExtensionErrc::invalid_profile, "invalid profile: " + reason);
}

void sort_and_check(std::vector<ProfileEntry>& extensions) {
    std::sort(extensions.begin(), extensions.end(),
              [](const ProfileEntry& a, const ProfileEntry& b) { return a.id < b.id; });
    for (std::size_t i = 0; i < extensions.size(); ++i) {
        if (!is_valid_extension_id(extensions[i].id)) {
            fail("'" + extensions[i].id + "' is not a module id");
        }
        if (i > 0 && extensions[i].id == extensions[i - 1].id) {
            fail("module '" + extensions[i].id + "' is listed twice");
        }
    }
}

}  // namespace

Profile parse_profile(std::string_view json) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error& e) {
        fail(std::string("not JSON: ") + e.what());
    }
    if (!document.is_object()) {
        fail("expected a JSON object");
    }
    constexpr std::array<std::string_view, 5> keys = {"extensions", "format", "formatVersion",
                                                      "layout", "name"};
    for (const auto& [key, value] : document.items()) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            fail("unknown key '" + key + "'");
        }
    }
    const auto format = document.find("format");
    if (format == document.end() || !format->is_string() ||
        format->get<std::string>() != format_name) {
        fail("'format' must be \"" + std::string(format_name) + "\"");
    }
    const auto version = document.find("formatVersion");
    if (version == document.end() || !version->is_number_integer() ||
        version->get<std::int64_t>() != format_version) {
        fail("'formatVersion' must be " + std::to_string(format_version));
    }
    Profile profile;
    const auto name = document.find("name");
    if (name == document.end() || !name->is_string()) {
        fail("'name' must be text");
    }
    profile.name = name->get<std::string>();
    const auto extensions = document.find("extensions");
    if (extensions == document.end() || !extensions->is_array()) {
        fail("'extensions' must be an array");
    }
    for (const nlohmann::json& entry : *extensions) {
        if (!entry.is_object() || entry.size() != 2 || !entry.contains("id") ||
            !entry.contains("version") || !entry["id"].is_string() ||
            !entry["version"].is_string()) {
            fail("an entry of 'extensions' must be {\"id\": \"...\", \"version\": \"...\"}");
        }
        try {
            profile.extensions.push_back({entry["id"].get<std::string>(),
                                          Version::parse(entry["version"].get<std::string>())});
        } catch (const ExtensionError& e) {
            fail(e.what());
        }
    }
    sort_and_check(profile.extensions);
    if (const auto layout = document.find("layout"); layout != document.end()) {
        if (!layout->is_object()) {
            fail("'layout' must be a JSON object");
        }
        profile.layout = *layout;
    }
    return profile;
}

std::string to_json(const Profile& profile) {
    std::vector<ProfileEntry> sorted = profile.extensions;
    sort_and_check(sorted);
    if (!profile.layout.is_object()) {
        fail("'layout' must be a JSON object");
    }
    nlohmann::json extensions = nlohmann::json::array();
    for (const ProfileEntry& entry : sorted) {
        extensions.push_back({{"id", entry.id}, {"version", entry.version.to_string()}});
    }
    // Objects of nlohmann::json keep their keys sorted, which makes the text deterministic.
    const nlohmann::json document = {
        {"extensions", std::move(extensions)},
        {"format", format_name},
        {"formatVersion", format_version},
        {"layout", profile.layout},
        {"name", profile.name},
    };
    try {
        return document.dump(2, ' ', false, nlohmann::json::error_handler_t::strict) + '\n';
    } catch (const nlohmann::json::type_error& e) {
        fail(std::string("text is not valid UTF-8: ") + e.what());
    }
}

Profile load_profile(const std::filesystem::path& file) {
    return parse_profile(detail::read_file(file));
}

void save_profile(const std::filesystem::path& file, const Profile& profile) {
    detail::write_file(file, to_json(profile));
}

}  // namespace fem::extension
