// Settings and translations follow the enabled modules; the language, the undo limit and the log
// level follow settings (task 1.7).

#include "fem/core/bindings.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/extension/manager.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/log/log.h"
#include "fem/core/model/document.h"
#include "fem/core/settings/error.h"
#include "fem/core/settings/settings.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::extension::ExtensionManager;
using fem::extension::ExtensionSource;
using fem::settings::Level;
using fem::settings::Settings;
using fem::settings::SettingsErrc;
using fem::settings::SettingsError;
using Keys = std::vector<std::string>;

/// tests/extensions/localized: test.app (settings and catalogs), test.pack (a language pack),
/// test.broken (catalogs that cannot be loaded).
fs::path localized() {
    return fs::path(FEM_TEST_EXTENSIONS_DIR) / "localized";
}

class BindingsTest : public ::testing::Test {
protected:
    BindingsTest() {
        const auto report = manager.scan(localized(), ExtensionSource::builtin);
        EXPECT_EQ(report.installed, (Keys{"test.app", "test.broken", "test.pack"}));
        EXPECT_TRUE(report.problems.empty());
    }

    fem::model::Document document;
    fem::command::CommandBus bus{document};
    fem::context::ContextKeys keys;
    ExtensionManager manager{document, bus, keys, {.engine = fem::extension::Version{1, 4, 0}}};
    Settings settings;
    fem::l10n::Localization l10n;
};

TEST_F(BindingsTest, SettingsOfAModuleComeAndGoWithIt) {
    const fem::Subscription binding = fem::bind_settings(manager, settings);
    EXPECT_EQ(settings.schema("app.threads"), nullptr);
    settings.load(Level::user, R"({"format": "stresslab.settings", "formatVersion": 1,
                                   "values": {"app.threads": 8, "general.language": "ru"}})");
    EXPECT_THROW((void)settings.value("app.threads"), SettingsError);

    Keys changed;
    const fem::Subscription log =
        settings.on_changed([&](std::string_view key) { changed.emplace_back(key); });
    manager.enable("test.app");
    EXPECT_EQ(settings.value("app.threads"), 8);
    EXPECT_EQ(settings.value("app.mode"), "fast");
    EXPECT_EQ(settings.schema("app.executable")->scope, fem::settings::Schema::Scope::user);
    EXPECT_EQ(changed, (Keys{"app.executable", "app.mode", "app.threads", "app.verbose"}));

    changed.clear();
    manager.disable("test.app");
    EXPECT_EQ(settings.schema("app.threads"), nullptr);
    EXPECT_EQ(settings.stored(Level::user, "app.threads"), nlohmann::json(8)) << "kept";
    EXPECT_EQ(changed.size(), 4U);
}

TEST_F(BindingsTest, SettingsOfModulesEnabledBeforeAreAddedAtOnce) {
    manager.enable("test.app");
    fem::Subscription binding = fem::bind_settings(manager, settings);
    EXPECT_EQ(settings.value("app.threads"), 2);
    // Resetting the subscription ends the following; what was added stays.
    binding.reset();
    manager.disable("test.app");
    EXPECT_NE(settings.schema("app.threads"), nullptr);
}

TEST_F(BindingsTest, CatalogsOfAModuleComeAndGoWithIt) {
    const fem::Subscription binding = fem::bind_catalogs(manager, l10n);
    l10n.set_language("ru");
    EXPECT_EQ(l10n.manifest_text("test.app", "%app.name%"), "app.name");
    manager.enable("test.app");
    const auto& manifest = *manager.get("test.app").manifest;
    EXPECT_EQ(l10n.manifest_text("test.app", manifest.name), "Демонстрационный модуль");
    EXPECT_EQ(l10n.manifest_text("test.app", "%setting.threads%"), "Число потоков");
    EXPECT_EQ(l10n.text("test.app", "jobs.count", {{"count", std::int64_t{22}}}), "22 задания");
    EXPECT_EQ(l10n.text("test.app", "only.english"), "Only in English");
    l10n.set_language("en");
    EXPECT_EQ(l10n.manifest_text("test.app", "%app.name%"), "Demo module");
    manager.disable("test.app");
    EXPECT_FALSE(l10n.has_owner("test.app"));
    EXPECT_EQ(l10n.manifest_text("test.app", "%app.name%"), "app.name");
}

TEST_F(BindingsTest, LanguagePacksTranslateModulesAndTheCore) {
    manager.enable("test.app");
    manager.enable("test.pack");
    const fem::Subscription binding = fem::bind_catalogs(manager, l10n);
    l10n.set_language("ru");
    EXPECT_EQ(l10n.text("test.app", "only.english"), "Перевод из пакета");
    EXPECT_EQ(l10n.text("test.app", "setting.threads"), "Число потоков") << "own catalog first";
    l10n.set_language("de");
    EXPECT_EQ(l10n.text("fem.core", "settings.type.integer"), "eine ganze Zahl");
    EXPECT_EQ(l10n.text("fem.core", "settings.type.number"), "a number");
    EXPECT_EQ(l10n.languages(), (Keys{"de", "en", "ru"}));
    manager.disable("test.pack");
    EXPECT_EQ(l10n.text("fem.core", "settings.type.integer"), "an integer");
}

TEST_F(BindingsTest, BrokenCatalogsAreReportedAndTheModuleIsEnabled) {
    const fem::Subscription settings_binding = fem::bind_settings(manager, settings);
    const fem::Subscription catalogs_binding = fem::bind_catalogs(manager, l10n);
    manager.enable("test.broken");
    EXPECT_TRUE(manager.get("test.broken").enabled());
    EXPECT_EQ(settings.value("broken.flag"), true);
    const auto& issues = l10n.issues();
    ASSERT_EQ(issues.size(), 2U);
    EXPECT_EQ(issues[0].owner, "test.broken");
    EXPECT_EQ(issues[0].language, "ru");
    EXPECT_NE(issues[0].message.find("not JSON"), std::string::npos) << issues[0].message;
    EXPECT_EQ(issues[1].language, "en");
    EXPECT_EQ(issues[1].file.filename(), "missing.json");
    manager.disable("test.broken");
    EXPECT_TRUE(l10n.issues().empty());
}

TEST_F(BindingsTest, TheLanguageFollowsTheSetting) {
    const fem::Subscription binding = fem::bind_language(settings, l10n);
    settings.set(Level::user, "general.language", "ru");
    EXPECT_EQ(l10n.language(), "ru");
    EXPECT_EQ(l10n.text("fem.core", "settings.type.number"), "числом");
    settings.set(Level::user, "general.language", "en-GB");
    EXPECT_EQ(l10n.language(), "en-GB");
    // Empty: the language of the system.
    settings.set(Level::user, "general.language", "");
    const std::string system = fem::l10n::system_language();
    EXPECT_EQ(l10n.language(), system.empty() ? "en" : system);
    // The language is a setting of the user only.
    EXPECT_THROW(settings.set(Level::project, "general.language", "ru"), SettingsError);
}

TEST_F(BindingsTest, TheUndoLimitFollowsTheSetting) {
    const fem::Subscription binding = fem::bind_undo_limit(settings, bus);
    EXPECT_EQ(bus.undo_limit(), 100U);
    settings.set(Level::user, "history.undoLimit", 5);
    EXPECT_EQ(bus.undo_limit(), 5U);
    settings.set(Level::user, "history.undoLimit", 0);
    EXPECT_EQ(bus.undo_limit(), 0U);
    try {
        settings.set(Level::user, "history.undoLimit", -1);
        FAIL() << "a negative limit was accepted";
    } catch (const SettingsError& e) {
        EXPECT_EQ(e.code(), SettingsErrc::below_minimum);
    }
}

TEST_F(BindingsTest, TheLogLevelFollowsTheSetting) {
    const fs::path folder = fs::temp_directory_path() / fs::path(u8"stresslab связки журнал");
    fs::remove_all(folder);
    {
        fem::log::Logging logging({.folder = folder});
        const fem::Subscription binding = fem::bind_log_level(settings, logging);
        EXPECT_EQ(logging.level(), fem::log::Level::info);
        settings.set(Level::user, "log.level", "debug");
        EXPECT_EQ(logging.level(), fem::log::Level::debug);
        settings.set(Level::user, "log.level", "warning");
        EXPECT_EQ(logging.level(), fem::log::Level::warn);
        EXPECT_THROW(settings.set(Level::user, "log.level", "loud"), SettingsError);
    }
    fs::remove_all(folder);
}

TEST_F(BindingsTest, ProblemsOfSettingsFilesAreLogged) {
    const fs::path folder = fs::temp_directory_path() / fs::path(u8"stresslab настройки журнал");
    fs::remove_all(folder);
    fs::path file;
    {
        fem::log::Logging logging({.folder = folder});
        file = logging.file();
        settings.load(Level::user, R"({"format": "stresslab.settings", "formatVersion": 1,
                                       "values": {"history.undoLimit": "many"}})");
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("[warning] [settings]"), std::string::npos) << text;
    EXPECT_NE(text.find("history.undoLimit"), std::string::npos) << text;
    in.close();
    fs::remove_all(folder);
}

}  // namespace
