#include "fem/core/l10n/localization.h"

#include "fem/core/l10n/error.h"
#include "fem/core/log/log.h"
#include "fem/core/model/object.h"

#include "../io/files.h"
#include "core_catalogs.h"
#include <fmt/format.h>

#include <algorithm>
#include <set>
#include <utility>

namespace fem::l10n {

std::string to_text(const Arg& arg) {
    if (const auto* text = std::get_if<std::string>(&arg)) {
        return *text;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&arg)) {
        return std::to_string(*integer);
    }
    // {fmt} writes the shortest text that reads back as the same double.
    return fmt::format("{}", std::get<double>(arg));
}

std::string format(std::string_view text, const Args& args) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c) {
            result += c;
            ++i;
            continue;
        }
        if (c == '{') {
            const std::size_t close = text.find('}', i + 1);
            if (close != std::string_view::npos) {
                const auto arg = args.find(text.substr(i + 1, close - i - 1));
                if (arg != args.end()) {
                    result += to_text(arg->second);
                    i = close;
                    continue;
                }
            }
        }
        result += c;
    }
    return result;
}

Localization::Localization() {
    for (const detail::EmbeddedCatalog& embedded : detail::core_catalogs()) {
        // The catalogs of the core are checked by its tests; a broken one is a build error.
        add_catalog(model::core_owner, embedded.language, parse_catalog(embedded.json));
    }
}

void Localization::set_language(std::string_view language) {
    std::string normalized = normalize_language(language);
    if (normalized.empty()) {
        normalized = std::string(fallback_language);
    }
    if (normalized == language_) {
        return;
    }
    language_ = std::move(normalized);
    chain_ = fallback_chain(language_);
    language_changed_.emit(language_);
}

std::vector<std::string> Localization::languages() const {
    std::set<std::string, std::less<>> languages;
    for (const auto& [owner, catalogs] : catalogs_) {
        for (const auto& [language, catalog] : catalogs) {
            languages.insert(language);
        }
    }
    return {languages.begin(), languages.end()};
}

void Localization::add_catalog(std::string_view owner, std::string_view language, Catalog catalog) {
    catalogs_[std::string(owner)].insert_or_assign(std::string(language), std::move(catalog));
}

bool Localization::add_catalog_file(std::string_view owner, std::string_view language,
                                    const std::filesystem::path& file) {
    try {
        add_catalog(owner, language, load_catalog(file));
        return true;
    } catch (const L10nError& e) {
        issues_.push_back({.owner = std::string(owner),
                           .language = std::string(language),
                           .file = file,
                           .message = e.what()});
        log::logger("l10n")->warn("the {} catalog of '{}' is not loaded: {}", language, owner,
                                  e.what());
        return false;
    }
}

bool Localization::remove_owner(std::string_view owner) {
    std::erase_if(issues_, [owner](const CatalogIssue& issue) { return issue.owner == owner; });
    const auto found = catalogs_.find(owner);
    if (found == catalogs_.end()) {
        return false;
    }
    catalogs_.erase(found);
    return true;
}

bool Localization::has_owner(std::string_view owner) const {
    return catalogs_.contains(owner);
}

std::optional<Localization::Found> Localization::lookup(std::string_view owner,
                                                        std::string_view key) const {
    const auto own = catalogs_.find(owner);
    for (const std::string& language : chain_) {
        if (own != catalogs_.end()) {
            const auto catalog = own->second.find(language);
            if (catalog != own->second.end()) {
                const auto message = catalog->second.strings.find(key);
                if (message != catalog->second.strings.end()) {
                    return Found{&message->second, language};
                }
            }
        }
        for (const auto& [pack, catalogs] : catalogs_) {
            if (pack == owner) {
                continue;
            }
            const auto catalog = catalogs.find(language);
            if (catalog == catalogs.end()) {
                continue;
            }
            const auto strings = catalog->second.modules.find(owner);
            if (strings == catalog->second.modules.end()) {
                continue;
            }
            const auto message = strings->second.find(key);
            if (message != strings->second.end()) {
                return Found{&message->second, language};
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> Localization::find(std::string_view owner, std::string_view key,
                                              const Args& args) const {
    const std::optional<Found> found = lookup(owner, key);
    if (!found) {
        return std::nullopt;
    }
    PluralCategory category = PluralCategory::other;
    if (found->message->is_plural()) {
        const auto count = args.find("count");
        if (count != args.end()) {
            category = plural_category(found->language, count->second);
        }
    }
    return format(found->message->form(category), args);
}

std::string Localization::text(std::string_view owner, std::string_view key,
                               const Args& args) const {
    std::optional<std::string> found = find(owner, key, args);
    return found ? std::move(*found) : std::string(key);
}

std::string Localization::manifest_text(std::string_view owner, std::string_view text,
                                        const Args& args) const {
    if (text.size() < 3 || text.front() != '%' || text.back() != '%') {
        return std::string(text);
    }
    const std::string_view key = text.substr(1, text.size() - 2);
    if (key.find('%') != std::string_view::npos) {
        return std::string(text);
    }
    return this->text(owner, key, args);
}

Subscription Localization::on_language_changed(
    std::function<void(const std::string& language)> listener) {
    return language_changed_.subscribe(std::move(listener));
}

}  // namespace fem::l10n
