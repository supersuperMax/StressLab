// Settings on the levels of the user and of a project, their files and events (task 1.7).

#include "fem/core/settings/settings.h"

#include "fem/core/extension/manifest.h"
#include "fem/core/settings/error.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::settings::Level;
using fem::settings::Schema;
using fem::settings::Settings;
using fem::settings::SettingsErrc;
using fem::settings::SettingsError;
using fem::settings::SettingState;
using nlohmann::json;
using Keys = std::vector<std::string>;

/// The settings of a module "test.solver", read through the manifest parser.
std::vector<Schema> solver_schemas() {
    const fem::extension::Manifest manifest = fem::extension::parse_manifest(
        R"([extension]
id = "test.solver"
name = "Solver"
version = "1.0.0"
engine = "^1.0"

[contributes.settings]
"solver.threads" = { type = "integer", default = 2, minimum = 1, maximum = 64 }
"solver.executable" = { type = "path", scope = "user" }
"solver.mode" = { type = "string", default = "fast", enum = ["fast", "exact"] }
"solver.order" = { type = "integer", enum = [2, 1] }
"solver.scale" = { type = "number" }
"solver.verbose" = { type = "boolean" }
)",
        fem::extension::Version{1, 0, 0});
    return manifest.contributes.settings;
}

/// Records the keys `on_changed` reports.
class ChangeLog {
public:
    explicit ChangeLog(Settings& settings)
        : subscription_(
              settings.on_changed([this](std::string_view key) { keys_.emplace_back(key); })) {}

    Keys take() { return std::exchange(keys_, {}); }

private:
    Keys keys_;
    fem::Subscription subscription_;
};

class SettingsTest : public ::testing::Test {
protected:
    SettingsTest() { settings.add_schemas(solver_schemas()); }

    Settings settings;
};

TEST(SettingsCore, HasTheSettingsOfTheCore) {
    const Settings settings;
    EXPECT_EQ(settings.value("general.language"), "");
    EXPECT_EQ(settings.get<std::int64_t>("history.undoLimit"), 100);
    EXPECT_EQ(settings.value("log.level"), "info");
    for (const Schema* schema : settings.schemas()) {
        EXPECT_EQ(schema->owner, "fem.core");
        EXPECT_EQ(schema->scope, Schema::Scope::user) << schema->key;
        EXPECT_EQ(schema->description, "%setting." + schema->key + "%");
    }
    EXPECT_TRUE(settings.issues().empty());
}

TEST_F(SettingsTest, DefaultsComeFromTheSchemas) {
    EXPECT_EQ(settings.value("solver.threads"), 2);
    EXPECT_EQ(settings.value("solver.mode"), "fast");
    // Without a default: the first allowed value, or false, 0 or "".
    EXPECT_EQ(settings.value("solver.order"), 2);
    EXPECT_EQ(settings.value("solver.executable"), "");
    EXPECT_EQ(settings.value("solver.scale"), 0);
    EXPECT_EQ(settings.value("solver.verbose"), false);
    EXPECT_EQ(settings.get<std::string>("solver.mode"), "fast");
}

TEST_F(SettingsTest, TheProjectOverridesTheUserWhoOverridesTheDefault) {
    settings.set(Level::user, "solver.threads", 8);
    EXPECT_EQ(settings.value("solver.threads"), 8);
    settings.set(Level::project, "solver.threads", 4);
    EXPECT_EQ(settings.value("solver.threads"), 4);

    const SettingState state = settings.state("solver.threads");
    ASSERT_NE(state.schema, nullptr);
    EXPECT_EQ(state.default_value, 2);
    EXPECT_EQ(state.user, json(8));
    EXPECT_EQ(state.project, json(4));
    EXPECT_EQ(state.value, 4);
    EXPECT_EQ(state.source, Level::project);

    EXPECT_TRUE(settings.reset(Level::project, "solver.threads"));
    EXPECT_FALSE(settings.reset(Level::project, "solver.threads"));
    EXPECT_EQ(settings.value("solver.threads"), 8);
    EXPECT_EQ(settings.state("solver.threads").source, Level::user);
    settings.clear(Level::user);
    EXPECT_EQ(settings.value("solver.threads"), 2);
    EXPECT_EQ(settings.state("solver.threads").source, std::nullopt);
}

TEST_F(SettingsTest, RefusedValuesChangeNothing) {
    settings.set(Level::user, "solver.threads", 8);
    const auto refused = [&](Level level, std::string_view key, const json& value,
                             SettingsErrc code) {
        try {
            settings.set(level, key, value);
            ADD_FAILURE() << key << " = " << value.dump() << " was accepted";
        } catch (const SettingsError& e) {
            EXPECT_EQ(e.code(), code) << e.what();
        }
    };
    refused(Level::user, "solver.threads", 0, SettingsErrc::below_minimum);
    refused(Level::user, "solver.threads", "8", SettingsErrc::wrong_type);
    refused(Level::user, "solver.mode", "slow", SettingsErrc::not_in_enum);
    refused(Level::project, "solver.executable", "/opt/solver", SettingsErrc::wrong_scope);
    refused(Level::user, "solver.unknown", 1, SettingsErrc::unknown_setting);
    EXPECT_EQ(settings.value("solver.threads"), 8);
    EXPECT_EQ(settings.keys(Level::user), Keys{"solver.threads"});
    EXPECT_TRUE(settings.keys(Level::project).empty());
    EXPECT_THROW((void)settings.value("solver.unknown"), SettingsError);
}

TEST_F(SettingsTest, ChangesAreReportedOnlyWhenTheValueInEffectChanges) {
    ChangeLog log(settings);
    settings.set(Level::user, "solver.threads", 8);
    EXPECT_EQ(log.take(), Keys{"solver.threads"});
    settings.set(Level::user, "solver.threads", 8);
    EXPECT_EQ(log.take(), Keys{});
    // The project sets what is in effect anyway.
    settings.set(Level::project, "solver.threads", 8);
    EXPECT_EQ(log.take(), Keys{});
    // Hidden by the project.
    settings.set(Level::user, "solver.threads", 16);
    EXPECT_EQ(log.take(), Keys{});
    settings.clear(Level::project);
    EXPECT_EQ(log.take(), Keys{"solver.threads"});
    settings.set(Level::user, "solver.mode", "fast");  // The default.
    EXPECT_EQ(log.take(), Keys{});
    EXPECT_TRUE(settings.reset(Level::user, "solver.threads"));
    EXPECT_EQ(log.take(), Keys{"solver.threads"});
}

TEST_F(SettingsTest, ListenersMayChangeSettings) {
    // A listener that keeps "solver.scale" equal to the number of threads.
    const fem::Subscription follow = settings.on_changed([this](std::string_view key) {
        if (key == "solver.threads") {
            settings.set(Level::user, "solver.scale", settings.value("solver.threads"));
        }
    });
    settings.set(Level::user, "solver.threads", 6);
    EXPECT_EQ(settings.value("solver.scale"), 6);
}

TEST_F(SettingsTest, ValuesWithoutASchemaAreKeptButNotInEffect) {
    settings.set(Level::user, "solver.threads", 8);
    settings.set(Level::project, "solver.mode", "exact");
    ChangeLog log(settings);
    EXPECT_TRUE(settings.remove_owner("test.solver"));
    EXPECT_FALSE(settings.remove_owner("test.solver"));
    // Every setting of the module changed: its value is gone.
    EXPECT_EQ(log.take(), (Keys{"solver.executable", "solver.mode", "solver.order", "solver.scale",
                                "solver.threads", "solver.verbose"}));
    EXPECT_EQ(settings.schema("solver.threads"), nullptr);
    EXPECT_THROW((void)settings.value("solver.threads"), SettingsError);
    const SettingState state = settings.state("solver.threads");
    EXPECT_EQ(state.schema, nullptr);
    EXPECT_EQ(state.user, json(8));
    EXPECT_TRUE(state.value.is_null());
    EXPECT_TRUE(settings.issues().empty()) << "a disabled module is not a problem";
    // Kept in the files and back in effect with the module.
    EXPECT_NE(settings.to_json(Level::user).find("\"solver.threads\": 8"), std::string::npos);
    settings.add_schemas(solver_schemas());
    EXPECT_EQ(settings.value("solver.threads"), 8);
    EXPECT_EQ(settings.value("solver.mode"), "exact");
    EXPECT_EQ(log.take(), (Keys{"solver.executable", "solver.mode", "solver.order", "solver.scale",
                                "solver.threads", "solver.verbose"}));
    // Resetting works without a schema too.
    settings.remove_owner("test.solver");
    EXPECT_TRUE(settings.reset(Level::project, "solver.mode"));
}

TEST_F(SettingsTest, InvalidValuesFromFilesAreKeptAndReported) {
    settings.load(Level::user, R"({
  "format": "stresslab.settings",
  "formatVersion": 1,
  "values": {"solver.threads": 1000, "solver.mode": "exact", "other.key": [1, 2]}
})");
    settings.load(Level::project, R"({
  "format": "stresslab.settings",
  "formatVersion": 1,
  "values": {"solver.executable": "/opt/solver", "solver.threads": "many"}
})");
    // Not in effect: the default and the user's valid values are.
    EXPECT_EQ(settings.value("solver.threads"), 2);
    EXPECT_EQ(settings.value("solver.executable"), "");
    EXPECT_EQ(settings.value("solver.mode"), "exact");

    const std::vector<fem::settings::Issue> issues = settings.issues();
    ASSERT_EQ(issues.size(), 3U);
    EXPECT_EQ(issues[0].key, "solver.executable");
    EXPECT_EQ(issues[0].code, SettingsErrc::wrong_scope);
    EXPECT_EQ(issues[1].key, "solver.threads");
    EXPECT_EQ(issues[1].level, Level::user);
    EXPECT_EQ(issues[1].code, SettingsErrc::above_maximum);
    EXPECT_EQ(issues[2].key, "solver.threads");
    EXPECT_EQ(issues[2].level, Level::project);
    EXPECT_EQ(issues[2].code, SettingsErrc::wrong_type);

    // Saved back as they were.
    EXPECT_EQ(settings.to_json(Level::user), R"({
  "format": "stresslab.settings",
  "formatVersion": 1,
  "values": {
    "other.key": [
      1,
      2
    ],
    "solver.mode": "exact",
    "solver.threads": 1000
  }
}
)");
    // Fixing the value removes the issue.
    settings.set(Level::user, "solver.threads", 64);
    EXPECT_EQ(settings.value("solver.threads"), 64);
    EXPECT_EQ(settings.issues().size(), 2U);
}

TEST_F(SettingsTest, SaveLoadSaveGivesTheSameBytes) {
    settings.set(Level::user, "solver.threads", 12);
    settings.set(Level::user, "solver.executable", "C:/Программы/My Solver/solver.exe");
    settings.set(Level::user, "solver.scale", 0.1);
    settings.set(Level::user, "solver.verbose", true);
    settings.set(Level::user, "general.language", "ru");
    const std::string first = settings.to_json(Level::user);

    Settings other;
    other.load(Level::user, first);
    EXPECT_EQ(other.to_json(Level::user), first);
    other.add_schemas(solver_schemas());
    EXPECT_EQ(other.value("solver.executable"), "C:/Программы/My Solver/solver.exe");
    EXPECT_EQ(other.value("solver.scale"), 0.1);
    EXPECT_EQ(other.to_json(Level::user), first);
    EXPECT_EQ(first.back(), '\n');
    EXPECT_EQ(first.find('\r'), std::string::npos);
}

TEST_F(SettingsTest, AnEmptyLevelIsAnEmptyFile) {
    EXPECT_EQ(settings.to_json(Level::project), R"({
  "format": "stresslab.settings",
  "formatVersion": 1,
  "values": {}
}
)");
}

TEST_F(SettingsTest, FilesInFoldersWithCyrillicLettersAndSpaces) {
    const fs::path folder = fs::temp_directory_path() / fs::path(u8"stresslab настройки test");
    fs::remove_all(folder);
    const fs::path file = folder / fs::path(u8"профиль пользователя") / "settings.json";

    EXPECT_FALSE(settings.load_file(Level::user, file)) << "no file yet: nothing to load";
    settings.set(Level::user, "solver.threads", 3);
    settings.save_file(Level::user, file);
    ASSERT_TRUE(fs::exists(file));

    Settings other;
    other.add_schemas(solver_schemas());
    EXPECT_TRUE(other.load_file(Level::user, file));
    EXPECT_EQ(other.value("solver.threads"), 3);
    // A missing file empties the level.
    EXPECT_FALSE(other.load_file(Level::user, folder / "missing.json"));
    EXPECT_EQ(other.value("solver.threads"), 2);
    fs::remove_all(folder);
}

TEST_F(SettingsTest, BadFilesAreRefusedAndChangeNothing) {
    settings.set(Level::user, "solver.threads", 8);
    const std::vector<std::string> bad = {
        "",
        "not json",
        "[]",
        R"({"format": "stresslab.profile", "formatVersion": 1, "values": {}})",
        R"({"format": "stresslab.settings", "formatVersion": 2, "values": {}})",
        R"({"format": "stresslab.settings", "formatVersion": 1})",
        R"({"format": "stresslab.settings", "formatVersion": 1, "values": []})",
        R"({"format": "stresslab.settings", "formatVersion": 1, "values": {}, "extra": 1})",
    };
    for (const std::string& text : bad) {
        try {
            settings.load(Level::user, text);
            ADD_FAILURE() << "accepted: " << text;
        } catch (const SettingsError& e) {
            EXPECT_EQ(e.code(), SettingsErrc::invalid_file) << text;
        }
    }
    EXPECT_EQ(settings.value("solver.threads"), 8);
}

TEST_F(SettingsTest, ASchemaWithATakenKeyIsRefused) {
    const std::vector<Schema> schemas = solver_schemas();
    Schema copy = *std::find_if(schemas.begin(), schemas.end(),
                                [](const Schema& s) { return s.key == "solver.threads"; });
    copy.owner = "test.other";
    Schema fresh = copy;
    fresh.key = "other.threads";
    settings.add_schemas({copy, fresh});
    EXPECT_EQ(settings.schema("solver.threads")->owner, "test.solver");
    EXPECT_EQ(settings.schema("other.threads")->owner, "test.other");
    const auto issues = settings.issues();
    ASSERT_EQ(issues.size(), 1U);
    EXPECT_EQ(issues[0].code, SettingsErrc::duplicate_setting);
    EXPECT_EQ(issues[0].key, "solver.threads");
    EXPECT_EQ(issues[0].level, std::nullopt);
    EXPECT_EQ(issues[0].args.at("owner"), fem::l10n::Arg(std::string("test.other")));
    // Removing the refused owner removes its issue.
    settings.remove_owner("test.other");
    EXPECT_TRUE(settings.issues().empty());
    // A module cannot take a setting of the core either.
    Schema language = copy;
    language.key = "general.language";
    settings.add_schemas({language});
    EXPECT_EQ(settings.schema("general.language")->owner, "fem.core");
}

}  // namespace
