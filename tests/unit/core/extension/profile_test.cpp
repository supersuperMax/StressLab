#include "fem/core/extension/profile.h"

#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/semver.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using fem::extension::ExtensionErrc;
using fem::extension::ExtensionSource;
using fem::extension::ExtensionState;
using fem::extension::load_profile;
using fem::extension::parse_profile;
using fem::extension::Profile;
using fem::extension::ProfileEntry;
using fem::extension::ProfileIssue;
using fem::extension::ProfileReport;
using fem::extension::save_profile;
using fem::extension::to_json;
using fem::extension::Version;
using fem::extension::testing::fixtures;
using fem::extension::testing::utf8;
using fem::extension::testing::Workspace;
using Ids = std::vector<std::string>;

Profile strength() {
    Profile profile;
    profile.name = utf8(u8"Прочность стали");
    profile.extensions = {{"test.solver", Version::parse("2.1.0")},
                          {"test.base", Version::parse("1.2.0-rc.1+b5")}};
    profile.layout = nlohmann::json::parse(
        R"({"docks": {"tree": {"width": 0.25, "visible": true}}, "order": [1, 2.5, "x"]})");
    return profile;
}

TEST(Profile, IsWrittenDeterministically) {
    const std::string text = to_json(strength());
    EXPECT_EQ(text, utf8(u8R"({
  "extensions": [
    {
      "id": "test.base",
      "version": "1.2.0-rc.1+b5"
    },
    {
      "id": "test.solver",
      "version": "2.1.0"
    }
  ],
  "format": "stresslab.profile",
  "formatVersion": 1,
  "layout": {
    "docks": {
      "tree": {
        "visible": true,
        "width": 0.25
      }
    },
    "order": [
      1,
      2.5,
      "x"
    ]
  },
  "name": "Прочность стали"
}
)"));
    // Save, load, save: the same bytes.
    const Profile parsed = parse_profile(text);
    EXPECT_EQ(to_json(parsed), text);
    EXPECT_EQ(parsed.name, utf8(u8"Прочность стали"));
    ASSERT_EQ(parsed.extensions.size(), 2U);
    EXPECT_EQ(parsed.extensions[0].id, "test.base");
    EXPECT_EQ(parsed.extensions[0].version.build(), "b5");
    EXPECT_EQ(parsed.layout["docks"]["tree"]["width"], 0.25);
}

TEST(Profile, IsSavedToAPathWithCyrillicLettersAndSpaces) {
    const fs::path folder = fs::temp_directory_path() / fs::path(u8"профили с пробелами");
    fs::create_directories(folder);
    const fs::path file = folder / fs::path(u8"Прочность стали.json");
    save_profile(file, strength());
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    EXPECT_EQ(bytes, to_json(strength()));
    Profile expected = strength();
    std::swap(expected.extensions[0], expected.extensions[1]);  // Loaded sorted by id.
    EXPECT_EQ(load_profile(file), expected);
    fs::remove_all(folder);
    EXPECT_EXTENSION_ERROR(static_cast<void>(load_profile(file)), ExtensionErrc::io);
}

TEST(Profile, RejectsWhatTheCoreDoesNotWrite) {
    const std::string valid = to_json(strength());
    const auto changed = [&](std::string_view from, std::string_view to) {
        std::string text = valid;
        text.replace(text.find(from), from.size(), to);
        return text;
    };
    for (const std::string& text : {std::string("not json"), std::string("[]"),
                                    changed("\"stresslab.profile\"", "\"other.profile\""),
                                    changed("\"formatVersion\": 1", "\"formatVersion\": 2"),
                                    changed("\"name\": ", "\"title\": "),
                                    changed("\"version\": \"2.1.0\"", "\"version\": \"2.1\""),
                                    changed("\"id\": \"test.solver\"", "\"id\": \"Test\""),
                                    changed("\"id\": \"test.solver\"", "\"id\": \"test.base\""),
                                    changed("\"layout\": {", "\"layout\": [{"),
                                    changed("\"format\"", "\"extra\": 1, \"format\"")}) {
        EXPECT_EXTENSION_ERROR(static_cast<void>(parse_profile(text)),
                               ExtensionErrc::invalid_profile);
    }
    Profile twice = strength();
    twice.extensions.push_back(twice.extensions.front());
    EXPECT_EXTENSION_ERROR(static_cast<void>(to_json(twice)), ExtensionErrc::invalid_profile);
}

class ProfileManagerTest : public ::testing::Test {
protected:
    ProfileManagerTest() { ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin); }

    Workspace ws;
};

TEST_F(ProfileManagerTest, ProfileListsTheEnabledModules) {
    ws.manager.enable("test.solver");
    const Profile profile =
        ws.manager.profile(utf8(u8"Прочность"), nlohmann::json({{"bottomPanel", "open"}}));
    EXPECT_EQ(profile.name, utf8(u8"Прочность"));
    EXPECT_EQ(profile.extensions, (std::vector<ProfileEntry>{{"test.base", Version(1, 2, 0)},
                                                             {"test.solver", Version(2, 1, 0)}}));
    EXPECT_EQ(profile.layout["bottomPanel"], "open");
}

TEST_F(ProfileManagerTest, ApplyingAProfileEnablesItsModulesAndDisablesTheOthers) {
    ws.manager.enable("test.base");
    ws.manager.enable("test.l10n");

    Profile profile;
    profile.name = "Mixed";
    profile.extensions = {{"test.future", Version(5, 0, 0)},
                          {"test.l10n", Version(0, 9, 0)},
                          {"test.missing", Version(1, 0, 0)},
                          {"test.post", Version(0, 3, 0)}};
    // test.future cannot be enabled, test.missing is not installed: neither is planned.
    const fem::extension::ProfilePlan plan = ws.manager.plan_profile(profile);
    EXPECT_EQ(plan.enable, (Ids{"test.post", "test.solver"}));
    EXPECT_TRUE(plan.disable.empty());
    const ProfileReport report = ws.manager.apply_profile(profile);
    EXPECT_EQ(report.enabled, (Ids{"test.solver", "test.post"}));
    EXPECT_TRUE(report.disabled.empty());
    EXPECT_EQ(report.missing, Ids{"test.missing"});
    ASSERT_EQ(report.version_mismatches.size(), 1U);
    EXPECT_EQ(report.version_mismatches[0].id, "test.l10n");
    EXPECT_EQ(report.version_mismatches[0].message,
              "the profile has version 0.9.0, version 1.0.0 is installed");
    ASSERT_EQ(report.failed.size(), 1U);
    EXPECT_EQ(report.failed[0].id, "test.future");
    for (const char* id : {"test.base", "test.l10n", "test.solver", "test.post"}) {
        EXPECT_NE(ws.manager.get(id).state, ExtensionState::disabled) << id;
    }

    // A smaller profile disables what it does not list, keeping what its modules need.
    Profile minimal;
    minimal.name = "Minimal";
    minimal.extensions = {{"test.base", Version(1, 2, 0)}};
    const fem::extension::ProfilePlan smaller_plan = ws.manager.plan_profile(minimal);
    EXPECT_TRUE(smaller_plan.enable.empty());
    EXPECT_EQ(smaller_plan.disable, (Ids{"test.l10n", "test.post", "test.solver"}));
    const ProfileReport smaller = ws.manager.apply_profile(minimal);
    EXPECT_TRUE(smaller.enabled.empty());
    EXPECT_EQ(smaller.disabled, (Ids{"test.l10n", "test.post", "test.solver"}));
    EXPECT_EQ(ws.manager.registry().owners(), Ids{"test.base"});

    // The profile of the manager is what was applied.
    EXPECT_EQ(ws.manager.profile("Minimal").extensions, minimal.extensions);
}

}  // namespace
