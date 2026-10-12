#include "fem/core/l10n/catalog.h"

#include "fem/core/l10n/error.h"

#include "../io/files.h"
#include <nlohmann/json.hpp>

#include <array>
#include <utility>

namespace fem::l10n {

namespace {

constexpr std::string_view format_name = "stresslab.l10n";
constexpr int format_version = 1;

constexpr std::array<PluralCategory, 6> categories = {
    PluralCategory::zero, PluralCategory::one,  PluralCategory::two,
    PluralCategory::few,  PluralCategory::many, PluralCategory::other,
};

[[noreturn]] void fail(const std::string& message) {
    throw L10nError(L10nErrc::invalid_catalog, message);
}

bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// "string 'a'" or "string 'a' of module 'test.x'".
std::string describe(std::string_view key, std::string_view module) {
    std::string where = "string '" + std::string(key) + "'";
    if (!module.empty()) {
        where += " of module '" + std::string(module) + "'";
    }
    return where;
}

std::string checked_text(const nlohmann::json& value, std::string_view key,
                         std::string_view module) {
    if (!value.is_string()) {
        fail(describe(key, module) + " must be text or an object of plural forms, not " +
             value.type_name());
    }
    std::string text = value.get<std::string>();
    try {
        (void)placeholders(text);
    } catch (const L10nError& e) {
        fail(describe(key, module) + ": " + e.what());
    }
    return text;
}

Message parse_message(const nlohmann::json& value, std::string_view key, std::string_view module) {
    Message message;
    if (!value.is_object()) {
        message.forms.emplace(PluralCategory::other, checked_text(value, key, module));
        return message;
    }
    for (const auto& [name, form] : value.items()) {
        const std::optional<PluralCategory> category = parse_plural_category(name);
        if (!category) {
            fail(describe(key, module) + ": unknown plural category '" + name +
                 "'; expected zero, one, two, few, many or other");
        }
        message.forms.emplace(*category, checked_text(form, key, module));
    }
    if (!message.forms.contains(PluralCategory::other)) {
        fail(describe(key, module) + ": plural forms need 'other'");
    }
    return message;
}

Strings parse_strings(const nlohmann::json& value, std::string_view where,
                      std::string_view module) {
    if (!value.is_object()) {
        fail(std::string(where) + " must be an object of strings, not " + value.type_name());
    }
    Strings strings;
    for (const auto& [key, message] : value.items()) {
        if (key.empty()) {
            fail(std::string(where) + " has an empty key");
        }
        strings.emplace(key, parse_message(message, key, module));
    }
    return strings;
}

}  // namespace

std::string_view to_string(PluralCategory category) noexcept {
    switch (category) {
        case PluralCategory::zero:
            return "zero";
        case PluralCategory::one:
            return "one";
        case PluralCategory::two:
            return "two";
        case PluralCategory::few:
            return "few";
        case PluralCategory::many:
            return "many";
        case PluralCategory::other:
            return "other";
    }
    return "other";
}

std::optional<PluralCategory> parse_plural_category(std::string_view name) noexcept {
    for (const PluralCategory category : categories) {
        if (to_string(category) == name) {
            return category;
        }
    }
    return std::nullopt;
}

const std::string& Message::form(PluralCategory category) const {
    const auto found = forms.find(category);
    return found != forms.end() ? found->second : forms.at(PluralCategory::other);
}

std::set<std::string, std::less<>> placeholders(std::string_view text) {
    std::set<std::string, std::less<>> names;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '}') {
            if (i + 1 < text.size() && text[i + 1] == '}') {
                ++i;
                continue;
            }
            throw L10nError(
                L10nErrc::invalid_catalog,
                "a '}' without '{'; write '}}' for a brace in \"" + std::string(text) + "\"");
        }
        if (c != '{') {
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '{') {
            ++i;
            continue;
        }
        const std::size_t close = text.find('}', i + 1);
        const std::string_view name = close == std::string_view::npos
                                          ? std::string_view()
                                          : text.substr(i + 1, close - i - 1);
        bool valid = !name.empty();
        for (const char n : name) {
            valid = valid && is_name_char(n);
        }
        if (!valid) {
            throw L10nError(L10nErrc::invalid_catalog,
                            "a broken placeholder in \"" + std::string(text) +
                                "\"; expected {name} of letters, digits and '_', or '{{' for a "
                                "brace");
        }
        names.emplace(name);
        i = close;
    }
    return names;
}

Catalog parse_catalog(std::string_view json) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error& e) {
        fail(std::string("not JSON: ") + e.what());
    }
    if (!document.is_object()) {
        fail("a catalog is not an object");
    }
    const auto format = document.find("format");
    if (format == document.end() || !format->is_string() ||
        format->get<std::string>() != format_name) {
        fail("'format' must be \"" + std::string(format_name) + "\"");
    }
    const auto version = document.find("formatVersion");
    if (version == document.end() || !version->is_number_integer() || *version != format_version) {
        fail("'formatVersion' must be " + std::to_string(format_version));
    }
    Catalog catalog;
    for (const auto& [key, value] : document.items()) {
        if (key == "format" || key == "formatVersion") {
            continue;
        }
        if (key == "strings") {
            catalog.strings = parse_strings(value, "'strings'", {});
        } else if (key == "modules") {
            if (!value.is_object()) {
                fail("'modules' must be an object of strings by module id, not " +
                     std::string(value.type_name()));
            }
            for (const auto& [module, strings] : value.items()) {
                catalog.modules.emplace(
                    module,
                    parse_strings(strings, "the strings of module '" + module + "'", module));
            }
        } else {
            fail("unknown key '" + key + "'; expected format, formatVersion, strings, modules");
        }
    }
    return catalog;
}

Catalog load_catalog(const std::filesystem::path& file) {
    const std::optional<std::string> bytes = io::read_file(file);
    if (!bytes) {
        throw L10nError(L10nErrc::io, "cannot read '" + io::display(file) + "'");
    }
    try {
        return parse_catalog(*bytes);
    } catch (const L10nError& e) {
        throw L10nError(e.code(), "'" + io::display(file) + "': " + e.what());
    }
}

}  // namespace fem::l10n
