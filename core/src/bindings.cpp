#include "fem/core/bindings.h"

#include "fem/core/extension/error.h"
#include "fem/core/settings/error.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace fem {

namespace {

/// Calls `apply` for every enabled module now and on "extension.enabled" later, and `remove` on
/// "extension.deactivated".
Subscription follow_modules(
    extension::ExtensionManager& manager,
    std::function<void(const extension::ExtensionInfo&, const extension::Contributions&)> apply,
    std::function<void(const std::string&)> remove) {
    for (const extension::ExtensionInfo* info : manager.extensions()) {
        if (const extension::Contributions* contributions = manager.registry().find(info->id())) {
            apply(*info, *contributions);
        }
    }
    return manager.on_event([&manager, apply = std::move(apply),
                             remove = std::move(remove)](const extension::ExtensionEvent& event) {
        using Kind = extension::ExtensionEvent::Kind;
        if (event.kind == Kind::enabled) {
            if (const extension::Contributions* contributions = manager.registry().find(event.id)) {
                apply(manager.get(event.id), *contributions);
            }
        } else if (event.kind == Kind::deactivated) {
            remove(event.id);
        }
    });
}

/// Calls `apply` now and whenever the setting `key` changes.
Subscription follow_setting(settings::Settings& settings, std::string_view key,
                            std::function<void()> apply) {
    apply();
    return settings.on_changed(
        [key = std::string(key), apply = std::move(apply)](std::string_view changed) {
            if (changed == key) {
                apply();
            }
        });
}

}  // namespace

Subscription bind_settings(extension::ExtensionManager& manager, settings::Settings& settings) {
    // The code of modules reads the values in effect.
    manager.set_setting_reader([&settings](std::string_view key) {
        try {
            return settings.value(key);
        } catch (const settings::SettingsError& error) {
            throw extension::ExtensionError(extension::ExtensionErrc::unknown_setting,
                                            error.what());
        }
    });
    auto schemas = std::make_shared<Subscription>(follow_modules(
        manager,
        [&settings](const extension::ExtensionInfo&,
                    const extension::Contributions& contributions) {
            settings.add_schemas(contributions.settings);
        },
        [&settings](const std::string& id) { settings.remove_owner(id); }));
    return Subscription([&manager, schemas] {
        schemas->reset();
        manager.set_setting_reader({});
    });
}

Subscription bind_catalogs(extension::ExtensionManager& manager, l10n::Localization& localization) {
    return follow_modules(
        manager,
        [&localization](const extension::ExtensionInfo& info,
                        const extension::Contributions& contributions) {
            for (const extension::LocalizationContribution& catalog : contributions.localization) {
                const std::filesystem::path relative(
                    std::u8string(catalog.path.begin(), catalog.path.end()));
                localization.add_catalog_file(info.id(), catalog.language, info.folder / relative);
            }
        },
        [&localization](const std::string& id) { localization.remove_owner(id); });
}

Subscription bind_language(settings::Settings& settings, l10n::Localization& localization,
                           std::string system_language) {
    if (system_language.empty()) {
        system_language = l10n::system_language();
    }
    return follow_setting(settings, settings::keys::language,
                          [&settings, &localization, system = std::move(system_language)] {
                              const std::string language =
                                  settings.get<std::string>(settings::keys::language);
                              localization.set_language(language.empty() ? system : language);
                          });
}

Subscription bind_undo_limit(settings::Settings& settings, command::CommandBus& bus) {
    return follow_setting(settings, settings::keys::undo_limit, [&settings, &bus] {
        const auto limit = settings.get<std::int64_t>(settings::keys::undo_limit);
        bus.set_undo_limit(limit < 0 ? 0 : static_cast<std::size_t>(limit));
    });
}

Subscription bind_log_level(settings::Settings& settings, log::Logging& logging) {
    return follow_setting(settings, settings::keys::log_level, [&settings, &logging] {
        const auto name = settings.get<std::string>(settings::keys::log_level);
        logging.set_level(log::parse_level(name).value_or(log::Level::info));
    });
}

}  // namespace fem
