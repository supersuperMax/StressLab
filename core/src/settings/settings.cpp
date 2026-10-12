#include "fem/core/settings/settings.h"

#include "fem/core/log/log.h"

#include "../io/files.h"

#include <algorithm>
#include <map>
#include <set>
#include <system_error>
#include <tuple>
#include <utility>

namespace fem::settings {

namespace {

constexpr std::string_view format_name = "stresslab.settings";
constexpr int format_version = 1;

[[noreturn]] void invalid_file(const std::string& reason) {
    throw SettingsError(SettingsErrc::invalid_file, "not a settings file: " + reason);
}

std::optional<Level> next_lower(std::optional<Level> level) {
    if (level == Level::project) {
        return Level::user;
    }
    return std::nullopt;
}

}  // namespace

Settings::Settings() {
    for (Schema& schema : core_schemas()) {
        std::string key = schema.key;
        schemas_.emplace(std::move(key), std::move(schema));
    }
}

// --- Schemas -------------------------------------------------------------------------------------

void Settings::add_schemas(const std::vector<Schema>& schemas) {
    std::vector<const Schema*> accepted;
    std::set<std::string, std::less<>> taken;
    for (const Schema& schema : schemas) {
        if (schemas_.contains(schema.key) || taken.contains(schema.key)) {
            const Schema* holder = this->schema(schema.key);
            const std::string other = holder != nullptr ? holder->owner : schema.owner;
            Issue refused;
            refused.code = SettingsErrc::duplicate_setting;
            refused.key = schema.key;
            refused.message = "setting '" + schema.key + "' of '" + schema.owner +
                              "' is refused: '" + other + "' already has it";
            refused.args = {{"key", schema.key}, {"owner", schema.owner}};
            log::logger("settings")->warn("{}", refused.message);
            refused_.push_back(std::move(refused));
            continue;
        }
        taken.insert(schema.key);
        accepted.push_back(&schema);
    }
    const std::vector<std::string> keys(taken.begin(), taken.end());
    changing(keys, [&] {
        for (const Schema* schema : accepted) {
            schemas_.emplace(schema->key, *schema);
        }
    });
    log_issues(keys, std::nullopt);
}

bool Settings::remove_owner(std::string_view owner) {
    std::vector<std::string> keys;
    for (const auto& [key, schema] : schemas_) {
        if (schema.owner == owner) {
            keys.push_back(key);
        }
    }
    const std::size_t refused = std::erase_if(refused_, [owner](const Issue& issue) {
        const auto found = issue.args.find("owner");
        return found != issue.args.end() && l10n::to_text(found->second) == owner;
    });
    changing(keys, [&] {
        for (const std::string& key : keys) {
            schemas_.erase(key);
        }
    });
    return !keys.empty() || refused > 0;
}

const Schema* Settings::schema(std::string_view key) const {
    const auto found = schemas_.find(key);
    return found == schemas_.end() ? nullptr : &found->second;
}

std::vector<const Schema*> Settings::schemas() const {
    std::vector<const Schema*> result;
    result.reserve(schemas_.size());
    for (const auto& [key, schema] : schemas_) {
        result.push_back(&schema);
    }
    return result;
}

// --- Values --------------------------------------------------------------------------------------

Settings::Values& Settings::values(Level level) {
    return level == Level::user ? user_ : project_;
}

const Settings::Values& Settings::values(Level level) const {
    return level == Level::user ? user_ : project_;
}

nlohmann::json Settings::default_value(const Schema& schema) {
    if (!schema.default_value.is_null()) {
        return schema.default_value;
    }
    if (const auto values = schema.schema.find("enum");
        values != schema.schema.end() && values->is_array() && !values->empty()) {
        return values->front();
    }
    switch (schema.type) {
        case Schema::Type::boolean:
            return false;
        case Schema::Type::integer:
        case Schema::Type::number:
            return 0;
        case Schema::Type::string:
        case Schema::Type::path:
            return "";
    }
    return nullptr;
}

nlohmann::json Settings::effective(std::string_view key) const {
    const Schema* found = schema(key);
    if (found == nullptr) {
        return nullptr;
    }
    return state(key).value;
}

nlohmann::json Settings::value(std::string_view key) const {
    if (schema(key) == nullptr) {
        throw SettingsError(SettingsErrc::unknown_setting,
                            "there is no setting '" + std::string(key) + "'");
    }
    return effective(key);
}

SettingState Settings::state(std::string_view key) const {
    SettingState result;
    result.schema = schema(key);
    result.user = stored(Level::user, key);
    result.project = stored(Level::project, key);
    if (result.schema == nullptr) {
        return result;
    }
    result.default_value = default_value(*result.schema);
    result.value = result.default_value;
    for (std::optional<Level> level = Level::project; level; level = next_lower(level)) {
        const std::optional<nlohmann::json>& value =
            *level == Level::project ? result.project : result.user;
        if (value && !validate(*result.schema, *value, *level)) {
            result.value = *value;
            result.source = level;
            break;
        }
    }
    return result;
}

std::optional<nlohmann::json> Settings::stored(Level level, std::string_view key) const {
    const Values& map = values(level);
    const auto found = map.find(key);
    if (found == map.end()) {
        return std::nullopt;
    }
    // In place: a json converts to anything, an optional too.
    return std::optional<nlohmann::json>(std::in_place, found->second);
}

std::vector<std::string> Settings::keys(Level level) const {
    std::vector<std::string> result;
    for (const auto& [key, value] : values(level)) {
        result.push_back(key);
    }
    return result;
}

void Settings::set(Level level, std::string_view key, nlohmann::json value) {
    const Schema* found = schema(key);
    if (found == nullptr) {
        throw SettingsError(SettingsErrc::unknown_setting,
                            "there is no setting '" + std::string(key) + "'");
    }
    if (const std::optional<Issue> issue = validate(*found, value, level)) {
        throw SettingsError(issue->code, issue->message);
    }
    changing({std::string(key)},
             [&] { values(level).insert_or_assign(std::string(key), std::move(value)); });
}

bool Settings::reset(Level level, std::string_view key) {
    Values& map = values(level);
    const auto found = map.find(key);
    if (found == map.end()) {
        return false;
    }
    changing({std::string(key)}, [&] { map.erase(found); });
    return true;
}

void Settings::clear(Level level) {
    changing(keys(level), [&] { values(level).clear(); });
}

// --- Files ---------------------------------------------------------------------------------------

void Settings::load(Level level, std::string_view json) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error& e) {
        invalid_file(std::string("not JSON: ") + e.what());
    }
    if (!document.is_object()) {
        invalid_file("not an object");
    }
    const auto format = document.find("format");
    if (format == document.end() || !format->is_string() ||
        format->get<std::string>() != format_name) {
        invalid_file("'format' must be \"" + std::string(format_name) + "\"");
    }
    const auto version = document.find("formatVersion");
    if (version == document.end() || !version->is_number_integer() || *version != format_version) {
        invalid_file("'formatVersion' must be " + std::to_string(format_version));
    }
    const auto loaded = document.find("values");
    if (loaded == document.end() || !loaded->is_object()) {
        invalid_file("'values' must be an object");
    }
    for (const auto& [key, value] : document.items()) {
        if (key != "format" && key != "formatVersion" && key != "values") {
            invalid_file("unknown key '" + key + "'; expected format, formatVersion, values");
        }
    }
    Values replacement;
    for (const auto& [key, value] : loaded->items()) {
        replacement.emplace(key, value);
    }
    std::set<std::string, std::less<>> affected;
    for (const auto& [key, value] : values(level)) {
        affected.insert(key);
    }
    std::vector<std::string> fresh;
    for (const auto& [key, value] : replacement) {
        affected.insert(key);
        fresh.push_back(key);
    }
    changing({affected.begin(), affected.end()}, [&] { values(level) = std::move(replacement); });
    log_issues(fresh, level);
}

std::string Settings::to_json(Level level) const {
    nlohmann::json stored_values = nlohmann::json::object();
    for (const auto& [key, value] : values(level)) {
        stored_values[key] = value;
    }
    // Objects of nlohmann::json keep their keys sorted, which makes the text deterministic.
    const nlohmann::json document = {
        {"format", format_name},
        {"formatVersion", format_version},
        {"values", std::move(stored_values)},
    };
    return document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
}

bool Settings::load_file(Level level, const std::filesystem::path& file) {
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        clear(level);
        return false;
    }
    const std::optional<std::string> bytes = io::read_file(file);
    if (!bytes) {
        throw SettingsError(SettingsErrc::io, "cannot read '" + io::display(file) + "'");
    }
    try {
        load(level, *bytes);
    } catch (const SettingsError& e) {
        log::logger("settings")->error("'{}': {}", io::display(file), e.what());
        throw SettingsError(e.code(), "'" + io::display(file) + "': " + e.what());
    }
    return true;
}

void Settings::save_file(Level level, const std::filesystem::path& file) const {
    std::error_code error;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), error);
    }
    if (error || !io::write_file(file, to_json(level))) {
        throw SettingsError(SettingsErrc::io, "cannot write '" + io::display(file) + "'");
    }
}

// --- Problems and events -------------------------------------------------------------------------

std::vector<Issue> Settings::issues() const {
    std::vector<Issue> result = refused_;
    for (const auto& [key, schema] : schemas_) {
        for (const Level level : {Level::user, Level::project}) {
            if (const std::optional<nlohmann::json> value = stored(level, key)) {
                if (std::optional<Issue> issue = validate(schema, *value, level)) {
                    result.push_back(std::move(*issue));
                }
            }
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const Issue& a, const Issue& b) {
        return std::tie(a.key, a.level) < std::tie(b.key, b.level);
    });
    return result;
}

Subscription Settings::on_changed(std::function<void(std::string_view key)> listener) {
    return changed_.subscribe(std::move(listener));
}

void Settings::changing(const std::vector<std::string>& affected,
                        const std::function<void()>& change) {
    std::map<std::string, nlohmann::json, std::less<>> before;
    for (const std::string& key : affected) {
        before.emplace(key, effective(key));
    }
    change();
    for (const auto& [key, previous] : before) {
        if (effective(key) != previous) {
            changed_.emit(key);
        }
    }
}

void Settings::log_issues(const std::vector<std::string>& keys, std::optional<Level> level) const {
    for (const std::string& key : keys) {
        const Schema* found = schema(key);
        if (found == nullptr) {
            continue;
        }
        for (const Level each : {Level::user, Level::project}) {
            if (level && each != *level) {
                continue;
            }
            if (const std::optional<nlohmann::json> value = stored(each, key)) {
                if (const std::optional<Issue> issue = validate(*found, *value, each)) {
                    log::logger("settings")->warn("{}; the value is ignored", issue->message);
                }
            }
        }
    }
}

}  // namespace fem::settings
