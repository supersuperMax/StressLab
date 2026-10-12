#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

// String catalogs: the translations of the core and of modules (`contributes.localization`), one
// JSON file per language (docs/adr/0008-settings-logging-l10n.md):
//
//     {
//       "format": "stresslab.l10n",
//       "formatVersion": 1,
//       "strings": {
//         "run.title": "Запустить расчёт",
//         "jobs.count": {"one": "{count} задание", "few": "{count} задания",
//                        "many": "{count} заданий", "other": "{count} задания"}
//       },
//       "modules": {"other.module": {"hello.title": "Привет"}}
//     }
//
// `strings` are the strings of the owner of the catalog. `modules`, optional, translates strings of
// other modules: a language pack is a module whose catalogs only have `modules`. A string is text
// with placeholders `{name}` (`{{` and `}}` for braces), or an object of plural forms chosen by the
// argument `count`; "other" is required, and the categories are those of CLDR.
namespace fem::l10n {

/// Plural categories of CLDR.
enum class PluralCategory : std::uint8_t { zero, one, two, few, many, other };

/// "zero", "one", "two", "few", "many", "other".
[[nodiscard]] std::string_view to_string(PluralCategory category) noexcept;
[[nodiscard]] std::optional<PluralCategory> parse_plural_category(std::string_view name) noexcept;

/// A translated string: one text, or texts by plural category ("other" always there).
struct Message {
    std::map<PluralCategory, std::string> forms;

    /// The text for a category, or the "other" form if the message has no such form.
    [[nodiscard]] const std::string& form(PluralCategory category) const;
    [[nodiscard]] bool is_plural() const noexcept { return forms.size() > 1; }

    friend bool operator==(const Message&, const Message&) = default;
};

using Strings = std::map<std::string, Message, std::less<>>;

/// The strings of one language from one file.
struct Catalog {
    Strings strings;
    /// Strings for other modules, by module id.
    std::map<std::string, Strings, std::less<>> modules;

    friend bool operator==(const Catalog&, const Catalog&) = default;
};

/// Reads a catalog. Throws L10nError(invalid_catalog) with the reason: not JSON, another format,
/// a string that is not text or plural forms, a plural form without "other" or of an unknown
/// category, a broken placeholder ("{", "{a b}").
[[nodiscard]] Catalog parse_catalog(std::string_view json);
/// Throws L10nError(io) if the file cannot be read, and as `parse_catalog` with the file named.
[[nodiscard]] Catalog load_catalog(const std::filesystem::path& file);

/// The names of the placeholders of a text, sorted, each once. Throws L10nError(invalid_catalog)
/// for a broken one.
[[nodiscard]] std::set<std::string, std::less<>> placeholders(std::string_view text);

}  // namespace fem::l10n
