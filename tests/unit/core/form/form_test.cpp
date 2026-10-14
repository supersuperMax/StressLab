// Forms from schemas: what the generator of the shell draws, checked on fixtures of
// tests/data/forms. Each `<name>.schema.json` has a golden `<name>.form.json` with the spec and the
// mistakes found in the schema; STRESSLAB_UPDATE_GOLDEN=1 writes them again (review the diff).

#include "fem/core/form/form.h"

#include "fem/core/extension/contributions.h"
#include "fem/core/l10n/catalog.h"
#include "fem/core/l10n/localization.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using fem::form::Field;
using fem::form::FieldKind;
using fem::form::FormSpec;
using fem::form::Problem;
using fem::form::ProblemCode;
using fem::form::SchemaError;
using fem::form::Severity;
using Json = nlohmann::json;

namespace {

const std::filesystem::path forms_dir = std::filesystem::path(FEM_TEST_DATA_DIR) / "forms";

Json read_json(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    EXPECT_TRUE(in) << file;
    std::stringstream text;
    text << in.rdbuf();
    return Json::parse(text.str());
}

FormSpec fixture(const std::string& name, std::vector<SchemaError>* errors = nullptr) {
    return fem::form::parse(read_json(forms_dir / (name + ".schema.json")), errors);
}

std::vector<std::string> codes(const std::vector<Problem>& problems) {
    std::vector<std::string> out;
    for (const Problem& problem : problems) {
        out.push_back(problem.pointer + ":" + std::string(fem::form::to_string(problem.code)));
    }
    return out;
}

using Strings = std::vector<std::string>;

/// STRESSLAB_UPDATE_GOLDEN is set: write the golden files instead of comparing.
bool update_golden() {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    const bool set = _dupenv_s(&value, &size, "STRESSLAB_UPDATE_GOLDEN") == 0 && value != nullptr;
    std::free(value);
    return set;
#else
    return std::getenv("STRESSLAB_UPDATE_GOLDEN") != nullptr;  // NOLINT(concurrency-mt-unsafe)
#endif
}

// Text as UTF-8 bytes. GoogleTest from vcpkg is built as C++17 and cannot print char8_t strings.
std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

}  // namespace

TEST(FormGolden, EveryFixtureReadsAsItsGoldenForm) {
    const bool update = update_golden();
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(forms_dir)) {
        const std::string file = entry.path().filename().string();
        const std::string suffix = ".schema.json";
        if (file.size() <= suffix.size() || !file.ends_with(suffix)) {
            continue;
        }
        ++count;
        const std::string name = file.substr(0, file.size() - suffix.size());
        std::vector<SchemaError> errors;
        const FormSpec form = fixture(name, &errors);
        Json actual{{"form", fem::form::to_json(form)}, {"errors", Json::array()}};
        for (const SchemaError& error : errors) {
            actual["errors"].push_back({{"pointer", error.pointer}, {"message", error.message}});
        }
        const std::filesystem::path golden = forms_dir / (name + ".form.json");
        if (update) {
            std::ofstream(golden, std::ios::binary) << actual.dump(2) << '\n';
            continue;
        }
        ASSERT_TRUE(std::filesystem::exists(golden)) << golden;
        EXPECT_EQ(actual.dump(2), read_json(golden).dump(2)) << name;
    }
    EXPECT_GE(count, 6);
}

TEST(FormParse, FieldsFollowTheirOrderThenTheirKeys) {
    const FormSpec form = fixture("material");
    Strings keys;
    for (const Field& field : form.fields) {
        keys.push_back(field.key);
    }
    EXPECT_EQ(keys, (Strings{"E", "nu", "density", "reference", "comment"}));
    EXPECT_EQ(form.fields[0].quantity, fem::model::find_quantity("pressure"));
    EXPECT_TRUE(form.fields[0].required);
    EXPECT_TRUE(form.fields[3].advanced);
}

TEST(FormParse, GroupsNestAndPointersEscapeNames) {
    const FormSpec form = fixture("groups");
    ASSERT_NE(fem::form::find_field(form, "solver/tolerance"), nullptr);
    const Field* escaped = fem::form::find_field(form, "solver/a~1b");
    ASSERT_NE(escaped, nullptr);
    EXPECT_EQ(escaped->key, "a/b");
    EXPECT_EQ(fem::form::find_field(form, "solver")->kind, FieldKind::group);
    EXPECT_EQ(fem::form::all_fields(form).size(), 7U);
    EXPECT_EQ(fem::form::find_field(form, "nothing"), nullptr);
}

TEST(FormParse, WhatFormsCannotShowIsRawWithoutErrors) {
    std::vector<SchemaError> errors;
    const FormSpec form = fixture("unsupported", &errors);
    EXPECT_TRUE(errors.empty());
    for (const Field& field : form.fields) {
        EXPECT_EQ(field.kind, FieldKind::raw) << field.key;
    }
}

TEST(FormParse, ARootThatIsNoObjectSchemaIsAnError) {
    for (const Json& schema : {Json(5), Json{{"type", "string"}}, Json::array()}) {
        std::vector<SchemaError> errors;
        EXPECT_TRUE(fem::form::parse(schema, &errors).fields.empty());
        EXPECT_EQ(errors.size(), 1U) << schema.dump();
    }
    // Without `type`, an object with properties is still a form.
    EXPECT_EQ(fem::form::parse({{"properties", {{"a", {{"type", "integer"}}}}}}).fields.size(), 1U);
}

TEST(FormCheck, RequiredFieldsMustNotBeEmpty) {
    const FormSpec form = fixture("material");
    EXPECT_EQ(codes(fem::form::check(form, Json::object())),
              (Strings{"E:required", "nu:required"}));
    EXPECT_EQ(codes(fem::form::check(form, {{"E", nullptr}, {"nu", 0.3}})),
              (Strings{"E:required"}));
    EXPECT_TRUE(fem::form::check(form, {{"E", 2.1e11}, {"nu", 0.3}, {"comment", ""}}).empty());
    // Empty text is no value: a required text field holding "" is empty.
    const FormSpec named = fem::form::parse({{"type", "object"},
                                             {"required", {"name"}},
                                             {"properties", {{"name", {{"type", "string"}}}}}});
    EXPECT_EQ(codes(fem::form::check(named, {{"name", ""}})), (Strings{"name:required"}));
}

TEST(FormCheck, ValuesMustFitTypesLimitsAndChoices) {
    const FormSpec material = fixture("material");
    EXPECT_EQ(codes(fem::form::check(material, {{"E", 0}, {"nu", 0.5}})),
              (Strings{"E:belowMinimum", "nu:aboveMaximum"}));
    EXPECT_EQ(codes(fem::form::check(material, {{"E", "high"}, {"nu", 0.0}, {"density", -1}})),
              (Strings{"E:wrongType", "density:belowMinimum"}));

    const FormSpec scalars = fixture("scalars");
    EXPECT_EQ(
        codes(
            fem::form::check(scalars, {{"enabled", 1}, {"count", 2.0}, {"name", 3}, {"limit", 0}})),
        (Strings{"count:wrongType", "enabled:wrongType", "limit:belowMinimum", "name:wrongType"}));
    EXPECT_TRUE(fem::form::check(scalars, {{"count", 100}, {"limit", 1}}).empty());

    const FormSpec choices = fixture("choices");
    EXPECT_EQ(codes(fem::form::check(choices, {{"method", "lu"}, {"order", 3}, {"mixed", 0}})),
              (Strings{"method:notInEnum", "order:notInEnum"}));
}

TEST(FormCheck, ClosedObjectsWarnAboutPropertiesTheyDoNotDescribe) {
    const FormSpec form = fixture("groups");
    const std::vector<Problem> problems = fem::form::check(
        form, {{"solver", {{"tolerance", 1e-6}, {"x", 1}}}, {"extra", true}, {"output", 5}});
    EXPECT_EQ(codes(problems),
              (Strings{"solver/x:unknownProperty", "output:wrongType", "extra:unknownProperty"}));
    EXPECT_EQ(problems[0].severity, Severity::warning);
    EXPECT_EQ(problems[1].severity, Severity::error);
    EXPECT_EQ(fem::form::worst(problems), Severity::error);
    EXPECT_EQ(fem::form::worst({}), Severity::ok);
    // A required group must be there, and what is inside it is checked.
    EXPECT_EQ(codes(fem::form::check(form, Json::object())), (Strings{"solver:required"}));
    EXPECT_EQ(codes(fem::form::check(form, {{"solver", Json::object()}})),
              (Strings{"solver/tolerance:required"}));
}

TEST(FormCheck, CheckValueSaysNothingForAGoodValue) {
    const FormSpec form = fixture("scalars");
    const Field& count = *fem::form::find_field(form, "count");
    EXPECT_FALSE(fem::form::check_value(count, 4));
    const auto problem = fem::form::check_value(count, 0);
    ASSERT_TRUE(problem);
    EXPECT_EQ(problem->code, ProblemCode::below_minimum);
    EXPECT_EQ(problem->detail, (Json{{"limit", 1.0}, {"exclusive", false}}));
    EXPECT_FALSE(fem::form::check_value(*fem::form::find_field(form, "file"), "a.txt"));
    EXPECT_TRUE(
        fem::form::check_value(*fem::form::find_field(form, "file"), std::string("a\0b", 3)));
    EXPECT_TRUE(fem::form::check_value(count, std::nan("")));
}

TEST(FormText, ProblemsReadInTheLanguageOfTheApplication) {
    fem::l10n::Localization localization;
    localization.add_catalog("test.module", "en",
                             fem::l10n::parse_catalog(
                                 R"({"format": "stresslab.l10n", "formatVersion": 1,
                "strings": {"material.E": "Young's modulus"}})"));
    const FormSpec form = fixture("material");
    const std::vector<Problem> problems = fem::form::check(form, {{"E", 0}, {"nu", 0.6}});
    ASSERT_EQ(problems.size(), 2U);
    EXPECT_EQ(fem::form::text(problems[0], localization, "test.module"),
              utf8(u8"“Young's modulus” must be greater than 0."));
    EXPECT_EQ(fem::form::text(problems[1], localization, "test.module"),
              utf8(u8"“Poisson's ratio” must be less than 0.5."));
    localization.set_language("ru");
    const std::vector<Problem> missing = fem::form::check(form, Json::object());
    EXPECT_EQ(fem::form::text(missing[1], localization, "test.module"),
              utf8(u8"Не задано обязательное поле «Poisson's ratio»."));
    const std::vector<Problem> typed = fem::form::check(form, {{"E", true}, {"nu", 0.1}});
    EXPECT_EQ(fem::form::text(typed[0], localization, "test.module"),
              utf8(u8"«Young's modulus» должно быть числом."));
}

TEST(SettingsForm, OneSectionPerOwnerWithTheSettingsByKey) {
    using fem::extension::SettingContribution;
    const auto setting = [](std::string owner, std::string key, SettingContribution::Type type,
                            Json schema) {
        SettingContribution result;
        result.owner = std::move(owner);
        result.key = std::move(key);
        result.type = type;
        result.default_value = schema.value("default", Json());
        result.description = schema.value("description", "");
        result.schema = std::move(schema);
        return result;
    };
    using Type = SettingContribution::Type;
    const std::vector<SettingContribution> settings{
        setting("fem.core", "log.level", Type::string,
                {{"default", "info"}, {"enum", {"info", "debug"}}, {"title", "%t%"}}),
        setting("fem.core", "history.undoLimit", Type::integer,
                {{"default", 100}, {"minimum", 0}, {"maximum", 1000}}),
        setting("test.solver", "solver.path", Type::path, {{"description", "Where it is"}}),
        setting("fem.core", "general.flag", Type::boolean, {{"default", false}}),
    };
    std::vector<const SettingContribution*> pointers;
    for (const SettingContribution& entry : settings) {
        pointers.push_back(&entry);
    }
    const FormSpec form = fem::form::settings_form(pointers);
    ASSERT_EQ(form.fields.size(), 2U);
    EXPECT_EQ(form.fields[0].kind, FieldKind::section);
    EXPECT_EQ(form.fields[0].key, "fem.core");
    EXPECT_EQ(form.fields[1].key, "test.solver");
    const std::vector<Field>& core = form.fields[0].fields;
    ASSERT_EQ(core.size(), 3U);
    EXPECT_EQ(core[0].key, "general.flag");
    EXPECT_EQ(core[0].kind, FieldKind::boolean);
    EXPECT_EQ(core[1].key, "history.undoLimit");
    EXPECT_EQ(core[1].pointer, "history.undoLimit");
    ASSERT_TRUE(core[1].minimum && core[1].maximum);
    EXPECT_EQ(*core[1].minimum, 0.0);
    EXPECT_EQ(*core[1].maximum, 1000.0);
    EXPECT_EQ(core[2].kind, FieldKind::choice);
    EXPECT_EQ(core[2].title, "%t%");
    EXPECT_EQ(core[2].choices.size(), 2U);
    EXPECT_EQ(form.fields[1].fields[0].kind, FieldKind::path);
    EXPECT_EQ(form.fields[1].fields[0].description, "Where it is");
    // Settings are checked by key, a section holds no value.
    EXPECT_EQ(fem::form::find_field(form, "history.undoLimit"), &core[1]);
    EXPECT_EQ(codes(fem::form::check(form, {{"history.undoLimit", -1}})),
              (Strings{"history.undoLimit:belowMinimum"}));
}
