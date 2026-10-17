#include "fem/shell/history_text.h"

#include "fem/shell/brand.h"
#include "fem/shell/strings.h"
#include "fem/storage/repository.h"

#include <QDateTime>
#include <array>
#include <exception>
#include <string>

namespace fem::shell {

namespace {

/// Colours of variants: blue for the main one, then green, purple, amber, teal, magenta, brown.
/// No grey: it is the colour of versions no variant reaches.
constexpr std::array<QRgb, 7> variant_colors{0x2F80ED, 0x27AE60, 0x9B51E0, 0xD9A21B,
                                             0x009E9E, 0xC2185B, 0xA0522D};

bool git_terms(const settings::Settings& settings) {
    try {
        return settings.get<bool>(settings::keys::git_terms);
    } catch (const std::exception&) {
        return false;  // Settings without the schemas of the core (tests).
    }
}

}  // namespace

QString history_text(const l10n::Localization& localization, const settings::Settings& settings,
                     std::string_view key, const l10n::Args& args) {
    if (git_terms(settings)) {
        const std::string git = std::string(key) + ".git";
        if (localization.find(strings_owner, git)) {
            return text(localization, git, args);
        }
    }
    return text(localization, key, args);
}

QString variant_title(const l10n::Localization& localization, const settings::Settings& settings,
                      std::string_view name) {
    if (name == storage::main_branch) {
        return history_text(localization, settings, "history.mainVariant");
    }
    return to_qstring(name);
}

QColor variant_color(std::string_view name) {
    if (name == storage::main_branch) {
        return QColor(variant_colors.front());
    }
    // FNV-1a of the UTF-8 bytes: the same on every computer and in every session.
    std::uint32_t hash = 2166136261U;
    for (const char c : name) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return QColor(variant_colors[1 + hash % (variant_colors.size() - 1)]);
}

QColor text_color_on(const QColor& background) {
    // Relative luminance, roughly: light backgrounds take graphite text.
    const double luminance =
        0.2126 * background.redF() + 0.7152 * background.greenF() + 0.0722 * background.blueF();
    return luminance > 0.55 ? brand::graphite : QColor(Qt::white);
}

QString version_time(const l10n::Localization& localization, std::int64_t seconds) {
    const QDateTime time = QDateTime::fromSecsSinceEpoch(seconds).toLocalTime();
    return time.toString(text(localization, "history.time"));
}

}  // namespace fem::shell
