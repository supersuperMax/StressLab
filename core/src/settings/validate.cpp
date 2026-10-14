// Checking values of settings against their schemas, and the schemas of the core.

#include "fem/core/model/object.h"
#include "fem/core/settings/settings.h"

#include "../model/utf8.h"

#include <cmath>
#include <string>
#include <utility>

namespace fem::settings {

namespace {

using Type = Schema::Type;

/// A value as JSON text for messages: "8", "\"fast\"".
std::string shown(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool fits_type(Type type, const nlohmann::json& value) {
    switch (type) {
        case Type::boolean:
            return value.is_boolean();
        case Type::integer:
            return value.is_number_integer();
        case Type::number:
            return value.is_number() && std::isfinite(value.get<double>());
        case Type::string:
        case Type::path: {
            if (!value.is_string()) {
                return false;
            }
            const auto& text = value.get_ref<const std::string&>();
            return model::detail::is_valid_utf8(text) &&
                   (type != Type::path || text.find('\0') == std::string::npos);
        }
    }
    return false;
}

Issue issue(SettingsErrc code, const Schema& schema, const nlohmann::json& value, Level level,
            const std::string& reason) {
    Issue result;
    result.code = code;
    result.key = schema.key;
    result.level = level;
    result.message = "setting '" + schema.key + "' in the " + std::string(to_string(level)) +
                     " settings: " + reason;
    result.args = {{"key", schema.key}, {"value", shown(value)}};
    return result;
}

Schema core_schema(std::string_view key, Type type, nlohmann::json schema) {
    Schema result;
    result.owner = std::string(model::core_owner);
    result.key = std::string(key);
    result.type = type;
    result.scope = Schema::Scope::user;
    result.description = "%setting." + result.key + "%";
    schema["type"] = std::string(extension::to_string(type));
    schema["scope"] = "user";
    schema["title"] = "%setting." + result.key + ".title%";
    schema["description"] = result.description;
    result.default_value = schema.at("default");
    result.schema = std::move(schema);
    return result;
}

}  // namespace

std::string_view to_string(Level level) noexcept {
    return level == Level::user ? "user" : "project";
}

std::vector<Schema> core_schemas() {
    return {
        core_schema(keys::language, Type::string, {{"default", ""}}),
        core_schema(keys::undo_limit, Type::integer,
                    {{"default", 100}, {"minimum", 0}, {"maximum", 1000000}}),
        core_schema(
            keys::log_level, Type::string,
            {{"default", "info"}, {"enum", {"trace", "debug", "info", "warning", "error"}}}),
    };
}

std::optional<Issue> validate(const Schema& schema, const nlohmann::json& value, Level level) {
    if (level == Level::project && schema.scope == Schema::Scope::user) {
        return issue(SettingsErrc::wrong_scope, schema, value, level,
                     "it can be set only in the settings of the user");
    }
    if (!fits_type(schema.type, value)) {
        Issue result = issue(
            SettingsErrc::wrong_type, schema, value, level,
            "expected " + std::string(extension::to_string(schema.type)) + ", not " + shown(value));
        result.args.emplace("type", std::string(extension::to_string(schema.type)));
        return result;
    }
    if (const auto values = schema.schema.find("enum"); values != schema.schema.end()) {
        bool listed = false;
        std::string allowed;
        for (const nlohmann::json& item : *values) {
            listed = listed || item == value;
            allowed += (allowed.empty() ? "" : ", ") + shown(item);
        }
        if (!listed) {
            Issue result = issue(SettingsErrc::not_in_enum, schema, value, level,
                                 shown(value) + " is not one of " + allowed);
            result.args.emplace("values", allowed);
            return result;
        }
    }
    if (schema.type == Type::integer || schema.type == Type::number) {
        const double number = value.get<double>();
        for (const auto& [bound, code] : {std::pair{"minimum", SettingsErrc::below_minimum},
                                          std::pair{"maximum", SettingsErrc::above_maximum}}) {
            const auto limit = schema.schema.find(bound);
            if (limit == schema.schema.end() || !limit->is_number()) {
                continue;
            }
            const double limit_value = limit->get<double>();
            if (code == SettingsErrc::below_minimum ? number < limit_value : number > limit_value) {
                Issue result =
                    issue(code, schema, value, level,
                          shown(value) + " is outside its " + bound + " " + shown(*limit));
                result.args.emplace(bound, shown(*limit));
                return result;
            }
        }
    }
    return std::nullopt;
}

std::string Issue::text(const l10n::Localization& localization) const {
    l10n::Args translated = args;
    if (const auto type = translated.find("type"); type != translated.end()) {
        type->second =
            localization.text(model::core_owner, "settings.type." + l10n::to_text(type->second));
    }
    return localization.text(model::core_owner, "settings.error." + std::string(to_string(code)),
                             translated);
}

}  // namespace fem::settings
