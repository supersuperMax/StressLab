#pragma once

#include "fem/core/l10n/catalog.h"
#include "fem/core/signal.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Translation of the text of the core and of modules, ru and en from the first day
// (docs/interface-design.md §10). Code asks for strings by owner and key; the shell translates
// manifest text ("%key%") when it builds menus and forms, so the language can change at any time.
namespace fem::l10n {

/// The language strings fall back to, and the language of the strings in code and manifests.
inline constexpr std::string_view fallback_language = "en";

/// A value for a placeholder.
using Arg = std::variant<std::string, std::int64_t, double>;
/// Values for the placeholders of a string, by name.
using Args = std::map<std::string, Arg, std::less<>>;

/// The text of an argument: text as it is, numbers in the shortest form that reads back the same,
/// with a '.' (numbers are not formatted by the language yet).
[[nodiscard]] std::string to_text(const Arg& arg);
/// Puts the arguments into the placeholders `{name}` of a text; `{{` and `}}` are braces. A
/// placeholder without an argument stays as it is, and so does a broken one.
[[nodiscard]] std::string format(std::string_view text, const Args& args);

/// The plural category of a number in a language, by the rules of CLDR for "en" and "ru" (and
/// "uk", "be"); other languages use the rules of English. A double with a fraction is "other".
[[nodiscard]] PluralCategory plural_category(std::string_view language, const Arg& count);

/// A language tag in the form catalogs use ("ru", "pt-BR", "zh-Hant"), from a tag or a POSIX
/// locale name ("ru_RU.UTF-8" → "ru-RU"); empty for "C", "POSIX" and what is not a language.
[[nodiscard]] std::string normalize_language(std::string_view tag);
/// The languages tried for a language: itself, its shorter prefixes, then "en":
/// "zh-Hant-TW" → zh-Hant-TW, zh-Hant, zh, en.
[[nodiscard]] std::vector<std::string> fallback_chain(std::string_view language);
/// The display language of the user: the UI language on Windows, LC_ALL, LC_MESSAGES or LANG
/// elsewhere; empty if unknown. (On macOS an application started from Finder has no LANG; the
/// shell passes the language of the system it reads from Qt.)
[[nodiscard]] std::string system_language();

/// A catalog that could not be loaded; the module works with the strings it had.
struct CatalogIssue {
    std::string owner;
    std::string language;
    std::filesystem::path file;
    std::string message;
};

/// Catalogs of the core and of the enabled modules, and the current language.
///
/// A string `key` of `owner` is looked up in the languages of `fallback_chain(language())`; in
/// each language, first in the catalog of the owner, then in the `modules` sections of other
/// catalogs (language packs) in the order of their owner ids. If no catalog has it, the key itself
/// is the text. Single-threaded, like the model.
class Localization {
public:
    /// Has the catalogs of the core (owner "fem.core"), built into the library; the language is
    /// "en".
    Localization();

    [[nodiscard]] const std::string& language() const noexcept { return language_; }
    /// Sets the language after `normalize_language`; an empty or unknown tag means "en". A
    /// language without catalogs is allowed: its strings fall back to English.
    void set_language(std::string_view language);
    /// Languages that have a catalog of the core or of a module, sorted, for the settings form.
    [[nodiscard]] std::vector<std::string> languages() const;

    /// Adds the catalog of `owner` for `language`, replacing the one there.
    void add_catalog(std::string_view owner, std::string_view language, Catalog catalog);
    /// Loads and adds a catalog file. A file that cannot be loaded is kept in `issues()` and in the
    /// log, and false is returned; the strings of the owner stay as they were.
    bool add_catalog_file(std::string_view owner, std::string_view language,
                          const std::filesystem::path& file);
    /// Removes the catalogs of a module and its issues; returns false if it had none.
    bool remove_owner(std::string_view owner);
    [[nodiscard]] bool has_owner(std::string_view owner) const;

    /// The text of a string in the current language with the arguments put in, or the key.
    [[nodiscard]] std::string text(std::string_view owner, std::string_view key,
                                   const Args& args = {}) const;
    /// The same, or nothing if no catalog has the string.
    [[nodiscard]] std::optional<std::string> find(std::string_view owner, std::string_view key,
                                                  const Args& args = {}) const;
    /// Text written in a manifest: "%key%" is the string `key` of `owner` (the key, without the
    /// percent signs, if no catalog has it); other text is returned as it is.
    [[nodiscard]] std::string manifest_text(std::string_view owner, std::string_view text,
                                            const Args& args = {}) const;

    [[nodiscard]] const std::vector<CatalogIssue>& issues() const noexcept { return issues_; }

    /// Called after the language changes, with the new one.
    [[nodiscard]] Subscription on_language_changed(
        std::function<void(const std::string& language)> listener);

private:
    struct Found {
        const Message* message;
        std::string_view language;
    };
    [[nodiscard]] std::optional<Found> lookup(std::string_view owner, std::string_view key) const;

    /// Catalogs by owner, then language.
    std::map<std::string, std::map<std::string, Catalog, std::less<>>, std::less<>> catalogs_;
    std::vector<CatalogIssue> issues_;
    std::string language_{fallback_language};
    std::vector<std::string> chain_{std::string(fallback_language)};
    Signal<const std::string&> language_changed_;
};

}  // namespace fem::l10n
