// Values of settings checked against their schemas (task 1.7).

#include "fem/core/extension/manifest.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/settings/error.h"
#include "fem/core/settings/settings.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fem::settings::Issue;
using fem::settings::Level;
using fem::settings::Schema;
using fem::settings::SettingsErrc;
using fem::settings::validate;
using nlohmann::json;

/// A schema as a module declares it, read through the manifest parser.
Schema schema(std::string_view entry) {
    const std::string text =
        "[extension]\nid = \"test.schemas\"\nname = \"Schemas\"\nversion = \"1.0.0\"\n"
        "engine = \"^1.0\"\n[contributes.settings]\n\"case.value\" = " +
        std::string(entry) + "\n";
    const fem::extension::Manifest manifest =
        fem::extension::parse_manifest(text, fem::extension::Version{1, 0, 0});
    return manifest.contributes.settings.at(0);
}

struct Case {
    const char* name;
    const char* schema;
    json value;
    std::optional<SettingsErrc> expected;  // None: the value fits.
};

std::ostream& operator<<(std::ostream& out, const Case& c) {
    return out << c.name;
}

class SettingsValidation : public ::testing::TestWithParam<Case> {};

TEST_P(SettingsValidation, ChecksTheValueAgainstTheSchema) {
    const Case& c = GetParam();
    const std::optional<Issue> issue = validate(schema(c.schema), c.value, Level::user);
    if (!c.expected) {
        EXPECT_FALSE(issue) << issue->message;
        return;
    }
    ASSERT_TRUE(issue) << c.value.dump();
    EXPECT_EQ(issue->code, *c.expected) << issue->message;
    EXPECT_EQ(issue->key, "case.value");
    EXPECT_EQ(issue->level, Level::user);
    EXPECT_NE(issue->message.find("case.value"), std::string::npos) << issue->message;
}

const char* const boolean = R"({ type = "boolean" })";
const char* const integer = R"({ type = "integer" })";
const char* const number = R"({ type = "number" })";
const char* const text = R"({ type = "string" })";
const char* const path = R"({ type = "path" })";
const char* const threads = R"({ type = "integer", minimum = 1, maximum = 64 })";
const char* const scale = R"({ type = "number", minimum = 0.5, maximum = 2.5 })";
const char* const mode = R"({ type = "string", enum = ["fast", "exact"] })";
const char* const order = R"({ type = "integer", enum = [1, 2, 4] })";

// clang-format off
INSTANTIATE_TEST_SUITE_P(
    Table, SettingsValidation,
    ::testing::Values(
        Case{"boolean true", boolean, true, std::nullopt},
        Case{"boolean false", boolean, false, std::nullopt},
        Case{"boolean from 1", boolean, 1, SettingsErrc::wrong_type},
        Case{"boolean from text", boolean, "true", SettingsErrc::wrong_type},
        Case{"boolean from null", boolean, nullptr, SettingsErrc::wrong_type},
        Case{"integer", integer, 42, std::nullopt},
        Case{"negative integer", integer, -7, std::nullopt},
        Case{"unsigned integer", integer, json(std::numeric_limits<std::uint64_t>::max()),
             std::nullopt},
        Case{"integer from a fraction", integer, 3.5, SettingsErrc::wrong_type},
        Case{"integer from a whole double", integer, 3.0, SettingsErrc::wrong_type},
        Case{"integer from a boolean", integer, true, SettingsErrc::wrong_type},
        Case{"integer from text", integer, "3", SettingsErrc::wrong_type},
        Case{"number from an integer", number, 3, std::nullopt},
        Case{"number from a double", number, 2.75, std::nullopt},
        Case{"number from infinity", number, std::numeric_limits<double>::infinity(),
             SettingsErrc::wrong_type},
        Case{"number from NaN", number, std::numeric_limits<double>::quiet_NaN(),
             SettingsErrc::wrong_type},
        Case{"number from text", number, "2.5", SettingsErrc::wrong_type},
        Case{"text", text, "Привет", std::nullopt},
        Case{"empty text", text, "", std::nullopt},
        Case{"text from a number", text, 5, SettingsErrc::wrong_type},
        Case{"text from an array", text, json::array({"a"}), SettingsErrc::wrong_type},
        Case{"text that is not UTF-8", text, std::string("\xff\xfe"), SettingsErrc::wrong_type},
        Case{"path", path, "D:/Расчёты/my solver/bin", std::nullopt},
        Case{"path from an object", path, json::object(), SettingsErrc::wrong_type},
        Case{"path with a zero byte", path, std::string("a\0b", 3), SettingsErrc::wrong_type},
        Case{"minimum itself", threads, 1, std::nullopt},
        Case{"maximum itself", threads, 64, std::nullopt},
        Case{"below the minimum", threads, 0, SettingsErrc::below_minimum},
        Case{"above the maximum", threads, 65, SettingsErrc::above_maximum},
        Case{"type before bounds", threads, 100.5, SettingsErrc::wrong_type},
        Case{"fractional bounds", scale, 0.5, std::nullopt},
        Case{"below a fractional minimum", scale, 0.49, SettingsErrc::below_minimum},
        Case{"above a fractional maximum", scale, 3, SettingsErrc::above_maximum},
        Case{"one of the enum", mode, "exact", std::nullopt},
        Case{"not in the enum", mode, "Fast", SettingsErrc::not_in_enum},
        Case{"enum of integers", order, 4, std::nullopt},
        Case{"not in an enum of integers", order, 3, SettingsErrc::not_in_enum}));
// clang-format on

TEST(SettingsValidation, AUserSettingIsRefusedInAProject) {
    const Schema user_only = schema(R"({ type = "path", scope = "user" })");
    EXPECT_FALSE(validate(user_only, "/usr/bin/solver", Level::user));
    const std::optional<Issue> issue = validate(user_only, "/usr/bin/solver", Level::project);
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->code, SettingsErrc::wrong_scope);
    EXPECT_EQ(issue->level, Level::project);

    const Schema shared = schema(R"({ type = "integer" })");
    EXPECT_FALSE(validate(shared, 4, Level::project));
}

TEST(SettingsValidation, IssuesCarryTheArgumentsOfTheirText) {
    const auto issue = validate(schema(R"({ type = "integer", maximum = 64 })"), 65, Level::user);
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->args.at("key"), fem::l10n::Arg(std::string("case.value")));
    EXPECT_EQ(issue->args.at("value"), fem::l10n::Arg(std::string("65")));
    EXPECT_EQ(issue->args.at("maximum"), fem::l10n::Arg(std::string("64")));

    const auto not_listed =
        validate(schema(R"({ type = "string", enum = ["fast", "exact"] })"), "slow", Level::user);
    ASSERT_TRUE(not_listed);
    EXPECT_EQ(not_listed->args.at("values"), fem::l10n::Arg(std::string(R"("fast", "exact")")));
    EXPECT_EQ(not_listed->args.at("value"), fem::l10n::Arg(std::string(R"("slow")")));
}

TEST(SettingsValidation, IssuesAreTranslated) {
    fem::l10n::Localization localization;
    const auto issue = validate(schema(R"({ type = "integer" })"), "четыре", Level::user);
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->text(localization), "“case.value” must be an integer, not \"четыре\".");
    localization.set_language("ru");
    EXPECT_EQ(issue->text(localization), "«case.value» должно быть целым числом, а не \"четыре\".");
}

}  // namespace
