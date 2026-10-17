#pragma once

#include "fem/core/extension/contributions.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/settings/error.h"
#include "fem/core/signal.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Settings of the application and of modules (docs/architecture.md §4.2 `settings`), on two
// levels: the user's (one file in the user's config folder) and the project's (a file of the
// project). See docs/adr/0008-settings-logging-l10n.md.
namespace fem::settings {

/// The schema of a setting: a `settings` entry of a manifest, or one of the core.
using Schema = extension::SettingContribution;

/// Keys of the settings of the core (owner "fem.core").
namespace keys {
/// The language of the interface; "" means the language of the system.
inline constexpr std::string_view language = "general.language";
/// How many steps can be undone; 0 means no limit.
inline constexpr std::string_view undo_limit = "history.undoLimit";
/// The lowest level written to the log: "trace", "debug", "info", "warning" or "error".
inline constexpr std::string_view log_level = "log.level";
/// Seconds between automatic saves of a project that has a folder; 0 turns them off.
inline constexpr std::string_view autosave_interval = "project.autosaveInterval";
/// Megabytes the cache of results of a project may take (task 2.5); 0 means no limit.
inline constexpr std::string_view result_cache_limit = "results.cacheLimit";
}  // namespace keys

/// The schemas of the settings of the core.
[[nodiscard]] std::vector<Schema> core_schemas();

/// Where a value is set. A value of the project overrides the user's, which overrides the default.
enum class Level : std::uint8_t { user, project };

/// "user", "project".
[[nodiscard]] std::string_view to_string(Level level) noexcept;

/// Why a value does not fit its schema, or a schema was refused.
struct Issue {
    SettingsErrc code = SettingsErrc::wrong_type;
    std::string key;
    /// The level of the value; none for a refused schema.
    std::optional<Level> level;
    /// For developers and logs.
    std::string message;
    /// The arguments of the text "settings.error.<code>" of the core catalogs: `key`, and as the
    /// code needs, `value` (as JSON), `type`, `values` (allowed, as JSON, joined by ", "),
    /// `minimum`, `maximum`, `owner`.
    l10n::Args args;

    /// The text for users in the language of `localization`.
    [[nodiscard]] std::string text(const l10n::Localization& localization) const;
};

/// Checks a value against a schema at a level; returns the problem, if any. Integers must be JSON
/// integers (3.0 is a number, not an integer), numbers must be finite, text and paths valid UTF-8.
[[nodiscard]] std::optional<Issue> validate(const Schema& schema, const nlohmann::json& value,
                                            Level level);

/// A setting as the settings form shows it.
struct SettingState {
    const Schema* schema = nullptr;  ///< Null for a value without a schema (module disabled).
    nlohmann::json default_value;    ///< Null without a schema.
    std::optional<nlohmann::json> user;
    std::optional<nlohmann::json> project;
    nlohmann::json value;         ///< In effect; null without a schema.
    std::optional<Level> source;  ///< Where `value` comes from; none for the default.
};

/// Values of the settings on both levels and the schemas they are checked against.
///
/// Nothing stored is lost: values without a schema (of a module that is disabled or not
/// installed) and values that do not fit theirs stay on their level and are saved back as they
/// are, but are not in effect; the level below (or the default) is. Such values are reported in
/// `issues()` and in the log. When the schema comes back, the value is checked again.
///
/// Settings are not the model: they change without the Command Bus, undo or the journal.
/// Single-threaded, like the model.
class Settings {
public:
    /// With the schemas of the core.
    Settings();

    // --- Schemas ---------------------------------------------------------------------------------

    /// Adds schemas. A schema with the key of another one already there is refused: reported in
    /// `issues()` (duplicate_setting) and the log, the others are added.
    void add_schemas(const std::vector<Schema>& schemas);
    /// Removes the schemas of an owner; returns false if it had none.
    bool remove_owner(std::string_view owner);
    /// The schema of a key, or nullptr.
    [[nodiscard]] const Schema* schema(std::string_view key) const;
    /// All schemas, sorted by key.
    [[nodiscard]] std::vector<const Schema*> schemas() const;

    // --- Values ----------------------------------------------------------------------------------

    /// The value in effect. Throws SettingsError(unknown_setting) for a key without a schema.
    [[nodiscard]] nlohmann::json value(std::string_view key) const;
    /// The value in effect as `T` (bool, std::int64_t, double, std::string).
    template <class T>
    [[nodiscard]] T get(std::string_view key) const {
        return value(key).template get<T>();
    }
    /// The default of a schema: its `default`, else the first of its `enum`, else false, 0 or "".
    [[nodiscard]] static nlohmann::json default_value(const Schema& schema);
    /// The setting with its values on each level. Works for keys without a schema too.
    [[nodiscard]] SettingState state(std::string_view key) const;
    /// The value stored on a level, as it is, valid or not.
    [[nodiscard]] std::optional<nlohmann::json> stored(Level level, std::string_view key) const;
    /// Keys with a value stored on a level, sorted.
    [[nodiscard]] std::vector<std::string> keys(Level level) const;

    /// Sets a value on a level. Throws SettingsError(unknown_setting) for a key without a schema
    /// and the code of `validate` for a value that does not fit; then nothing changes.
    void set(Level level, std::string_view key, nlohmann::json value);
    /// Removes the value of a level, with a schema or without; returns false if there was none.
    bool reset(Level level, std::string_view key);
    /// Removes all values of a level (a project was closed).
    void clear(Level level);

    // --- Files -----------------------------------------------------------------------------------

    /// Replaces the values of a level with those of a settings file's text:
    ///
    ///     {
    ///       "format": "stresslab.settings",
    ///       "formatVersion": 1,
    ///       "values": {"general.language": "ru", "solver.threads": 4}
    ///     }
    ///
    /// Values that do not fit their schemas are kept and reported, not refused. Throws
    /// SettingsError(invalid_file) for text that is not in this format; then nothing changes.
    void load(Level level, std::string_view json);
    /// The values of a level as a settings file, deterministically: keys sorted, two-space indent,
    /// UTF-8 as is, '\n' line breaks and one at the end. Loading it and writing it again gives the
    /// same bytes.
    [[nodiscard]] std::string to_json(Level level) const;
    /// Loads a level from a file; returns false and clears the level if there is no file. Throws
    /// SettingsError(io) if it cannot be read, and as `load`.
    bool load_file(Level level, const std::filesystem::path& file);
    /// Saves a level to a file, creating its folder. Throws SettingsError(io).
    void save_file(Level level, const std::filesystem::path& file) const;

    // --- Problems and events ---------------------------------------------------------------------

    /// Stored values that are not in effect because they do not fit their schemas, and refused
    /// schemas; sorted by key, then level.
    [[nodiscard]] std::vector<Issue> issues() const;

    /// Called with the key of every setting whose value in effect changed: by `set`, `reset`,
    /// `clear`, loading, or a schema added or removed. Listeners may change settings.
    [[nodiscard]] Subscription on_changed(std::function<void(std::string_view key)> listener);

private:
    using Values = std::map<std::string, nlohmann::json, std::less<>>;

    [[nodiscard]] Values& values(Level level);
    [[nodiscard]] const Values& values(Level level) const;
    /// The value in effect, or null for a key without a schema.
    [[nodiscard]] nlohmann::json effective(std::string_view key) const;
    /// Runs `change` and reports the keys of `affected` whose value in effect changed.
    void changing(const std::vector<std::string>& affected, const std::function<void()>& change);
    void log_issues(const std::vector<std::string>& keys, std::optional<Level> level) const;

    std::map<std::string, Schema, std::less<>> schemas_;
    std::vector<Issue> refused_;
    Values user_;
    Values project_;
    Signal<std::string_view> changed_;
};

}  // namespace fem::settings
