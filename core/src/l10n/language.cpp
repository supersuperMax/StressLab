// Language tags, their fallbacks, the language of the system and plural rules.

#include "fem/core/l10n/catalog.h"
#include "fem/core/l10n/localization.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fem::l10n {

namespace {

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

char upper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '-' || text[i] == '_') {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

/// The primary language of a tag: "pt" of "pt-BR".
std::string_view primary(std::string_view language) {
    return language.substr(0, language.find('-'));
}

/// The rules of Russian, Ukrainian and Belarusian for a whole number.
PluralCategory east_slavic(std::uint64_t n) {
    const std::uint64_t last = n % 10;
    const std::uint64_t last_two = n % 100;
    if (last == 1 && last_two != 11) {
        return PluralCategory::one;
    }
    if (last >= 2 && last <= 4 && (last_two < 12 || last_two > 14)) {
        return PluralCategory::few;
    }
    return PluralCategory::many;
}

}  // namespace

std::string normalize_language(std::string_view tag) {
    const std::size_t end = tag.find_first_of(".@");
    const std::vector<std::string_view> parts = split(tag.substr(0, end));
    std::string result;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const std::string_view part = parts[index];
        bool letters = !part.empty();
        bool alnum = !part.empty() && part.size() <= 8;
        for (const char c : part) {
            letters = letters && is_alpha(c);
            alnum = alnum && (is_alpha(c) || is_digit(c));
        }
        if (index == 0) {
            if (!letters || part.size() < 2 || part.size() > 3) {
                return {};
            }
        } else if (!alnum) {
            return {};
        }
        if (index > 0) {
            result += '-';
        }
        for (std::size_t i = 0; i < part.size(); ++i) {
            const bool region = index > 0 && letters && part.size() == 2;
            const bool script_initial = index > 0 && letters && part.size() == 4 && i == 0;
            result += (region || script_initial) ? upper(part[i]) : lower(part[i]);
        }
    }
    return result;
}

std::vector<std::string> fallback_chain(std::string_view language) {
    std::vector<std::string> chain;
    std::string_view rest = language;
    while (!rest.empty()) {
        chain.emplace_back(rest);
        const std::size_t dash = rest.rfind('-');
        rest = dash == std::string_view::npos ? std::string_view() : rest.substr(0, dash);
    }
    if (chain.empty() || chain.back() != fallback_language) {
        chain.emplace_back(fallback_language);
    }
    return chain;
}

std::string system_language() {
#ifdef _WIN32
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    const LANGID language = GetUserDefaultUILanguage();
    if (LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) <= 0) {
        return {};
    }
    std::string ascii;
    for (const wchar_t* c = name; *c != L'\0'; ++c) {
        if (*c > 0x7f) {
            return {};
        }
        ascii += static_cast<char>(*c);
    }
    return normalize_language(ascii);
#else
    for (const char* variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(variable);  // NOLINT(concurrency-mt-unsafe)
        if (value != nullptr && *value != '\0') {
            return normalize_language(value);
        }
    }
    return {};
#endif
}

PluralCategory plural_category(std::string_view language, const Arg& count) {
    std::uint64_t whole = 0;
    if (const auto* integer = std::get_if<std::int64_t>(&count)) {
        whole = *integer < 0 ? static_cast<std::uint64_t>(-(*integer + 1)) + 1
                             : static_cast<std::uint64_t>(*integer);
    } else if (const auto* number = std::get_if<double>(&count)) {
        const double magnitude = std::fabs(*number);
        if (!std::isfinite(magnitude) || magnitude != std::floor(magnitude) || magnitude >= 1e18) {
            return PluralCategory::other;
        }
        whole = static_cast<std::uint64_t>(magnitude);
    } else {
        return PluralCategory::other;
    }
    const std::string_view base = primary(language);
    if (base == "ru" || base == "uk" || base == "be") {
        return east_slavic(whole);
    }
    return whole == 1 ? PluralCategory::one : PluralCategory::other;
}

}  // namespace fem::l10n
