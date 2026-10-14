#include "fem/core/extension/user_modules.h"

#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/manifest.h"

#include "files.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <system_error>
#include <utility>

namespace fem::extension {

namespace {

constexpr std::string_view format_name = "stresslab.modules";
constexpr std::int64_t format_version = 1;

[[noreturn]] void fail(const std::string& reason) {
    throw ExtensionError(ExtensionErrc::invalid_profile, "invalid state of modules: " + reason);
}

std::filesystem::path from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

/// Sorts, drops repeats and checks the values.
void normalize(UserModules& modules) {
    std::sort(modules.disabled.begin(), modules.disabled.end());
    modules.disabled.erase(std::unique(modules.disabled.begin(), modules.disabled.end()),
                           modules.disabled.end());
    for (const std::string& id : modules.disabled) {
        if (!is_valid_extension_id(id)) {
            fail("'" + id + "' is not a module id");
        }
    }
    for (std::filesystem::path& folder : modules.local) {
        if (!folder.is_absolute()) {
            fail("the folder '" + detail::display(folder) + "' is not absolute");
        }
        folder = folder.lexically_normal();
    }
    std::sort(modules.local.begin(), modules.local.end());
    modules.local.erase(std::unique(modules.local.begin(), modules.local.end()),
                        modules.local.end());
}

}  // namespace

UserModules parse_user_modules(std::string_view json) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error& e) {
        fail(std::string("not JSON: ") + e.what());
    }
    if (!document.is_object()) {
        fail("expected a JSON object");
    }
    constexpr std::array<std::string_view, 4> keys = {"disabled", "format", "formatVersion",
                                                      "local"};
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
    UserModules modules;
    const auto texts = [&](std::string_view key) {
        std::vector<std::string> result;
        const auto list = document.find(key);
        if (list == document.end()) {
            return result;
        }
        if (!list->is_array()) {
            fail("'" + std::string(key) + "' must be an array of text");
        }
        for (const nlohmann::json& item : *list) {
            if (!item.is_string()) {
                fail("'" + std::string(key) + "' must be an array of text");
            }
            result.push_back(item.get<std::string>());
        }
        return result;
    };
    modules.disabled = texts("disabled");
    for (const std::string& folder : texts("local")) {
        modules.local.push_back(from_utf8(folder));
    }
    normalize(modules);
    return modules;
}

std::string to_json(const UserModules& modules) {
    UserModules sorted = modules;
    normalize(sorted);
    nlohmann::json local = nlohmann::json::array();
    for (const std::filesystem::path& folder : sorted.local) {
        local.push_back(detail::display(folder));
    }
    // Objects of nlohmann::json keep their keys sorted, which makes the text deterministic.
    const nlohmann::json document = {
        {"disabled", sorted.disabled},
        {"format", format_name},
        {"formatVersion", format_version},
        {"local", std::move(local)},
    };
    try {
        return document.dump(2, ' ', false, nlohmann::json::error_handler_t::strict) + '\n';
    } catch (const nlohmann::json::type_error& e) {
        fail(std::string("text is not valid UTF-8: ") + e.what());
    }
}

UserModules load_user_modules(const std::filesystem::path& file) {
    return parse_user_modules(detail::read_file(file));
}

void save_user_modules(const std::filesystem::path& file, const UserModules& modules) {
    const std::string text = to_json(modules);
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    detail::write_file(file, text);
}

UserModules user_modules(const ExtensionManager& manager) {
    UserModules modules;
    for (const ExtensionInfo* info : manager.extensions()) {
        if (info->compatible() && !info->enabled()) {
            modules.disabled.push_back(info->id());
        }
        if (info->source == ExtensionSource::local) {
            modules.local.push_back(info->folder);
        }
    }
    normalize(modules);
    return modules;
}

}  // namespace fem::extension
