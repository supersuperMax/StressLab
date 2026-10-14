#include "fem/core/extension/profile.h"

#include "fem/core/extension/error.h"
#include "fem/core/extension/manifest.h"

#include "files.h"

#include <algorithm>
#include <array>
#include <set>
#include <system_error>
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

// --- ProfileLibrary ------------------------------------------------------------------------------

namespace {

std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}  // namespace

ProfileLibrary::ProfileLibrary(std::filesystem::path folder) : folder_(std::move(folder)) {}

std::vector<std::pair<std::filesystem::path, Profile>> ProfileLibrary::read_all() const {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder_, error), end; !error && it != end;
         it.increment(error)) {
        std::error_code ignored;
        if (it->is_regular_file(ignored) && it->path().extension() == ".json") {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<std::pair<std::filesystem::path, Profile>> profiles;
    for (const std::filesystem::path& file : files) {
        try {
            profiles.emplace_back(file, load_profile(file));
        } catch (const ExtensionError&) {
            // Not a profile.
        }
    }
    return profiles;
}

std::vector<std::string> ProfileLibrary::names() const {
    std::set<std::string> names;
    for (const auto& [file, profile] : read_all()) {
        names.insert(profile.name);
    }
    return {names.begin(), names.end()};
}

std::optional<Profile> ProfileLibrary::find(std::string_view name) const {
    for (auto& [file, profile] : read_all()) {
        if (profile.name == name) {
            return std::move(profile);
        }
    }
    return std::nullopt;
}

std::filesystem::path ProfileLibrary::file_of(std::string_view name) const {
    for (const auto& [file, profile] : read_all()) {
        if (profile.name == name) {
            return file;
        }
    }
    return {};
}

std::filesystem::path ProfileLibrary::save(const Profile& profile) {
    std::filesystem::path file = file_of(profile.name);
    if (file.empty()) {
        std::error_code error;
        std::filesystem::create_directories(folder_, error);
        if (error) {
            throw ExtensionError(ExtensionErrc::io,
                                 "cannot create '" + detail::display(folder_) + "'");
        }
        // Another file may have the name already (a profile of a similar name, not a profile).
        file = folder_ / utf8_path(file_name(profile.name));
        for (int n = 2; std::filesystem::exists(file, error); ++n) {
            file = folder_ / utf8_path(file_name(profile.name + " (" + std::to_string(n) + ")"));
        }
    }
    save_profile(file, profile);
    return file;
}

bool ProfileLibrary::remove(std::string_view name) {
    const std::filesystem::path file = file_of(name);
    if (file.empty()) {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::remove(file, error) || error) {
        throw ExtensionError(ExtensionErrc::io, "cannot delete '" + detail::display(file) + "'");
    }
    return true;
}

std::string ProfileLibrary::file_name(std::string_view name) {
    std::string result;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        constexpr std::string_view forbidden = "<>:\"/\\|?*";
        result += byte < 0x20 || forbidden.find(c) != std::string_view::npos ? '_' : c;
    }
    // Windows drops dots and spaces at the end of a name; spaces at the start are easy to miss.
    while (!result.empty() && (result.back() == '.' || result.back() == ' ')) {
        result.pop_back();
    }
    result.erase(0, result.find_first_not_of(' '));
    if (result.empty()) {
        result = "profile";
    }
    // Names of devices on Windows, also with an extension ("con.txt").
    std::string base = result.substr(0, result.find('.'));
    std::transform(base.begin(), base.end(), base.begin(), [](char c) {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    });
    constexpr std::array<std::string_view, 4> devices = {"CON", "PRN", "AUX", "NUL"};
    const bool numbered = base.size() == 4 &&
                          (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '1' &&
                          base[3] <= '9';
    if (numbered || std::find(devices.begin(), devices.end(), base) != devices.end()) {
        result.insert(result.begin(), '_');
    }
    return result + ".json";
}

}  // namespace fem::extension
