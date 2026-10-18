#include "fem/core/extension/manifest.h"

#include "fem/core/command/command.h"
#include "fem/core/context/value.h"
#include "fem/core/extension/contributions.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/semver.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using fem::command::ArgType;
using fem::command::Value;
using fem::context::ContextValue;
using fem::extension::ActivationEvent;
using fem::extension::Contributions;
using fem::extension::ExtensionErrc;
using fem::extension::ExtensionKind;
using fem::extension::FileFormatContribution;
using fem::extension::load_manifest;
using fem::extension::Manifest;
using fem::extension::manifest_file_name;
using fem::extension::ManifestDiagnostic;
using fem::extension::ManifestError;
using fem::extension::parse_manifest;
using fem::extension::SettingContribution;
using fem::extension::StageContribution;
using fem::extension::Version;
using fem::extension::testing::engine;
using fem::extension::testing::fixtures;
using fem::extension::testing::utf8;

// The [extension] table most cases start with; its lines are 1 to 7.
constexpr std::string_view header = R"([extension]
id = "test.case"
name = "Case"
version = "1.0.0"
engine = "^1.0"
kind = "python"
entry = "case:activate"
)";

// The same without code: lines 1 to 5.
constexpr std::string_view header_without_code = R"([extension]
id = "test.case"
name = "Case"
version = "1.0.0"
engine = "^1.0"
)";

std::string with_header(std::string_view rest) {
    return std::string(header) + std::string(rest);
}

/// The diagnostics of a manifest that must not parse.
std::vector<ManifestDiagnostic> diagnostics(std::string_view text) {
    try {
        static_cast<void>(parse_manifest(text, engine));
    } catch (const ManifestError& error) {
        return error.diagnostics();
    }
    ADD_FAILURE() << "The manifest parsed:\n" << text;
    return {};
}

TEST(Manifest, ReadsEveryContributionPoint) {
    const Manifest manifest =
        load_manifest(fixtures() / "full" / std::string(manifest_file_name), engine);

    EXPECT_EQ(manifest.file, fixtures() / "full" / std::string(manifest_file_name));
    EXPECT_EQ(manifest.id, "test.full");
    EXPECT_EQ(manifest.name, utf8(u8"Полный модуль"));
    EXPECT_EQ(manifest.description, "Uses every contribution point");
    EXPECT_EQ(manifest.author, utf8(u8"Лаборатория прочности"));
    EXPECT_EQ(manifest.version.to_string(), "1.2.0-rc.1+build.7");
    EXPECT_EQ(manifest.engine.to_string(), "^1.0");
    ASSERT_TRUE(manifest.has_code());
    EXPECT_EQ(*manifest.kind, ExtensionKind::python);
    EXPECT_EQ(manifest.entry, "test_full.main:activate");
    ASSERT_EQ(manifest.requirements.size(), 3U);
    EXPECT_EQ(manifest.requirements[0].id, "test.base");
    EXPECT_EQ(manifest.requirements[0].range.to_string(), ">=1.0");
    EXPECT_EQ(manifest.requirements[1].id, "test.mesh");
    EXPECT_EQ(manifest.requirements[1].range.to_string(), "^2.1 || ^3");
    EXPECT_EQ(manifest.requirements[2].id, "test.any");
    EXPECT_EQ(manifest.requirements[2].range.to_string(), "*");
    EXPECT_EQ(manifest.activation,
              (std::vector<ActivationEvent>{ActivationEvent::startup(),
                                            ActivationEvent::command("full.*"),
                                            {ActivationEvent::Trigger::open_file, "*.RES"},
                                            ActivationEvent::solver_requested("full-solver")}));

    const Contributions& c = manifest.contributes;
    EXPECT_EQ(c.owner, "test.full");

    ASSERT_EQ(c.commands.size(), 2U);
    EXPECT_EQ(c.commands[0].owner, "test.full");
    EXPECT_EQ(c.commands[0].id, "full.run");
    EXPECT_EQ(c.commands[0].title, utf8(u8"Запустить расчёт"));
    EXPECT_EQ(c.commands[0].category, utf8(u8"Решение"));
    EXPECT_EQ(c.commands[0].icon, "icons/run.svg");
    ASSERT_TRUE(c.commands[0].enablement.has_value());
    EXPECT_EQ(c.commands[0].enablement->to_string(), "model.hasMesh");
    EXPECT_TRUE(c.commands[0].snapshot_before);
    const auto& args = c.commands[0].args;
    ASSERT_EQ(args.size(), 6U);
    EXPECT_EQ(args[0].name, "job");
    EXPECT_EQ(args[0].type, ArgType::string);
    EXPECT_TRUE(args[0].required);
    EXPECT_FALSE(args[0].default_value.has_value());
    EXPECT_EQ(args[1].name, "threads");
    EXPECT_FALSE(args[1].required);
    EXPECT_EQ(args[1].default_value, Value(0));
    // A default of a number argument is a number even when written as an integer.
    EXPECT_EQ(args[2].type, ArgType::number);
    EXPECT_EQ(args[2].default_value, Value(2.0));
    EXPECT_EQ(args[3].type, ArgType::path);
    EXPECT_FALSE(args[3].required);
    EXPECT_FALSE(args[3].default_value.has_value());
    EXPECT_EQ(args[4].type, ArgType::object);
    EXPECT_EQ(args[5].name, "options");
    EXPECT_EQ(args[5].type, ArgType::any);
    EXPECT_EQ(c.commands[1].id, "full.stop");
    EXPECT_TRUE(c.commands[1].args.empty());
    EXPECT_FALSE(c.commands[1].enablement.has_value());
    EXPECT_FALSE(c.commands[1].snapshot_before);

    ASSERT_EQ(c.stages.size(), 2U);
    EXPECT_EQ(c.stages[0].id, "full.optimize");
    EXPECT_EQ(c.stages[0].placement, StageContribution::Placement::after);
    EXPECT_EQ(c.stages[0].anchor, "results");
    ASSERT_TRUE(c.stages[0].when.has_value());
    EXPECT_EQ(c.stages[0].when->to_string(), "model.hasMesh");
    EXPECT_EQ(c.stages[1].placement, StageContribution::Placement::before);
    EXPECT_EQ(c.stages[1].anchor, "solve");
    EXPECT_FALSE(c.stages[1].when.has_value());

    ASSERT_EQ(c.tree_node_types.size(), 1U);
    EXPECT_EQ(c.tree_node_types[0].type, "full_job");
    EXPECT_EQ(c.tree_node_types[0].icon, "icons/job.svg");

    ASSERT_EQ(c.menus.size(), 1U);
    EXPECT_EQ(c.menus[0].location, "solve/run");
    EXPECT_EQ(c.menus[0].command, "full.run");
    EXPECT_EQ(c.menus[0].when->to_string(), "model.hasMesh && solver.full-solver.available");
    EXPECT_EQ(c.menus[0].group, "1_run@1");
    ASSERT_EQ(c.context_menus.size(), 1U);
    EXPECT_EQ(c.context_menus[0].location, "tree");
    EXPECT_EQ(c.context_menus[0].when->keys(), std::vector<std::string>{"tree.selectedNodeType"});
    ASSERT_EQ(c.submenus.size(), 1U);
    EXPECT_EQ(c.submenus[0].id, "optimize");
    EXPECT_EQ(c.submenus[0].title, utf8(u8"Оптимизация"));
    ASSERT_EQ(c.context_toolbar.size(), 1U);
    EXPECT_EQ(c.context_toolbar[0].command, "full.run");

    ASSERT_EQ(c.property_schemas.size(), 1U);
    EXPECT_EQ(c.property_schemas[0].type, "full_job");
    EXPECT_EQ(c.property_schemas[0].version, 2U);
    EXPECT_EQ(c.property_schemas[0].schema,
              nlohmann::json::parse(R"({"type": "object", "required": ["name"], "properties":
                  {"name": {"type": "string"}, "cpus": {"type": "integer", "minimum": 1}}})"));

    ASSERT_EQ(c.file_formats.size(), 1U);
    EXPECT_EQ(c.file_formats[0].id, "full-res");
    EXPECT_EQ(c.file_formats[0].extensions, (std::vector<std::string>{".res", ".res2"}));
    EXPECT_EQ(c.file_formats[0].role, FileFormatContribution::Role::results_import);

    ASSERT_EQ(c.solvers.size(), 1U);
    EXPECT_EQ(c.solvers[0].id, "full-solver");
    EXPECT_EQ(c.solvers[0].label, "Full Solver");
    EXPECT_EQ(c.solvers[0].analyses, (std::vector<std::string>{"static", "frequency"}));
    ASSERT_EQ(c.meshers.size(), 1U);
    EXPECT_EQ(c.meshers[0].parameters, "full_job");

    ASSERT_EQ(c.activity_bar.size(), 1U);
    EXPECT_EQ(c.activity_bar[0].icon, "icons/panel.svg");
    ASSERT_EQ(c.bottom_panel_tabs.size(), 1U);
    EXPECT_EQ(c.bottom_panel_tabs[0].title, utf8(u8"Монитор"));
    EXPECT_EQ(c.bottom_panel_tabs[0].when->to_string(), "job.running");
    ASSERT_EQ(c.viewport_layers.size(), 1U);
    EXPECT_EQ(c.viewport_layers[0].id, "full.modes");
    ASSERT_EQ(c.result_filters.size(), 1U);
    EXPECT_EQ(c.result_filters[0].id, "full.safety");
    ASSERT_EQ(c.result_indicators.size(), 2U);
    EXPECT_EQ(c.result_indicators[0].id, "full.mass");
    EXPECT_EQ(c.result_indicators[0].title, utf8(u8"Масса"));
    EXPECT_EQ(c.result_indicators[0].quantity, "mass");
    EXPECT_TRUE(c.result_indicators[1].quantity.empty());

    // Settings are a TOML table, so they come in the order of their keys.
    ASSERT_EQ(c.settings.size(), 3U);
    EXPECT_EQ(c.settings[0].key, "full.executable");
    EXPECT_EQ(c.settings[0].type, SettingContribution::Type::path);
    EXPECT_EQ(c.settings[0].default_value, "");
    EXPECT_EQ(c.settings[0].description, utf8(u8"Путь к решателю"));
    EXPECT_EQ(c.settings[0].scope, SettingContribution::Scope::user);
    EXPECT_EQ(c.settings[0].schema["scope"], "user");
    EXPECT_EQ(c.settings[1].key, "full.mode");
    EXPECT_EQ(c.settings[1].schema["enum"], nlohmann::json::parse(R"(["fast", "exact"])"));
    EXPECT_EQ(c.settings[2].key, "full.threads");
    EXPECT_EQ(c.settings[2].type, SettingContribution::Type::integer);
    EXPECT_EQ(c.settings[2].default_value, 0);
    EXPECT_EQ(c.settings[2].schema["maximum"], 256);
    EXPECT_EQ(c.settings[2].scope, SettingContribution::Scope::project);

    ASSERT_EQ(c.keybindings.size(), 1U);
    EXPECT_EQ(c.keybindings[0].key, "Mod+Shift+R");
    EXPECT_EQ(c.keybindings[0].when->to_string(), "stage == 'solve'");
    ASSERT_EQ(c.icons.size(), 1U);
    EXPECT_EQ(c.icons[0].path, "icons/run.svg");
    ASSERT_EQ(c.localization.size(), 2U);
    EXPECT_EQ(c.localization[1].language, "pt-BR");
    EXPECT_EQ(c.localization[1].path, "l10n/pt-BR.json");

    ASSERT_EQ(c.context_keys.size(), 3U);
    EXPECT_EQ(c.context_keys[0].name, "full.ready");
    EXPECT_EQ(c.context_keys[0].type, ContextValue::Kind::boolean);
    EXPECT_EQ(c.context_keys[0].initial, ContextValue(false));
    EXPECT_EQ(c.context_keys[1].initial, ContextValue(ContextValue::List{"a", utf8(u8"б")}));
    EXPECT_EQ(c.context_keys[2].type, ContextValue::Kind::number);
    EXPECT_TRUE(c.context_keys[2].initial.is_none());
    EXPECT_EQ(c.context_keys[0].spec().owner, "test.full");

    // Every record knows its module.
    for (const auto* owner :
         {&c.stages[0].owner,           &c.tree_node_types[0].owner, &c.menus[0].owner,
          &c.context_menus[0].owner,    &c.submenus[0].owner,        &c.context_toolbar[0].owner,
          &c.property_schemas[0].owner, &c.file_formats[0].owner,    &c.solvers[0].owner,
          &c.meshers[0].owner,          &c.activity_bar[0].owner,    &c.bottom_panel_tabs[0].owner,
          &c.viewport_layers[0].owner,  &c.result_filters[0].owner,  &c.result_indicators[0].owner,
          &c.settings[0].owner,         &c.keybindings[0].owner,     &c.icons[0].owner,
          &c.localization[0].owner,     &c.context_keys[0].owner}) {
        EXPECT_EQ(*owner, "test.full");
    }
}

TEST(Manifest, AnActivityMayGoWithoutAnIcon) {
    // The shell then draws the default module icon.
    const Manifest manifest = parse_manifest(
        with_header("[[contributes.activityBar]]\nid = \"case.a\"\ntitle = \"A\"\n"), engine);
    ASSERT_EQ(manifest.contributes.activity_bar.size(), 1U);
    EXPECT_EQ(manifest.contributes.activity_bar[0].icon, "");
}

TEST(Manifest, ModuleWithoutCodeHasNoKind) {
    const Manifest manifest =
        load_manifest(fixtures() / "installed" / "l10n" / std::string(manifest_file_name), engine);
    EXPECT_FALSE(manifest.has_code());
    EXPECT_TRUE(manifest.entry.empty());
    EXPECT_EQ(manifest.engine.to_string(), ">=1.0 <2");
    EXPECT_EQ(manifest.contributes.keybindings.size(), 1U);
}

TEST(Manifest, ModuleForAnotherPlatformIsReadOnlyByItsHeader) {
    const std::filesystem::path file =
        fixtures() / "installed" / "future" / std::string(manifest_file_name);
    // It has an unknown kind, an unknown key and an unknown contribution point...
    const Manifest manifest = load_manifest(file, engine);
    EXPECT_EQ(manifest.id, "test.future");
    EXPECT_EQ(manifest.version, Version(5, 0, 0));
    EXPECT_FALSE(manifest.engine.contains(engine));
    EXPECT_FALSE(manifest.has_code());
    EXPECT_TRUE(manifest.contributes.commands.empty());
    // ...which are errors for the platform it is made for.
    try {
        static_cast<void>(load_manifest(file, Version(99, 1, 0)));
        FAIL();
    } catch (const ManifestError& error) {
        // 'kind' without 'entry', the unknown kind, 'teleport' and 'holograms'.
        ASSERT_EQ(error.diagnostics().size(), 4U) << error.what();
        EXPECT_EQ(error.diagnostics()[0].line, 2U);
        EXPECT_EQ(error.diagnostics()[1].line, 7U);
        EXPECT_EQ(error.diagnostics()[2].line, 8U);
        EXPECT_EQ(error.diagnostics()[3].line, 10U);
    }
}

TEST(Manifest, ReportsTheLineOfAnUnknownKey) {
    const std::filesystem::path file =
        fixtures() / "installed" / "broken" / std::string(manifest_file_name);
    try {
        static_cast<void>(load_manifest(file, engine));
        FAIL();
    } catch (const ManifestError& error) {
        EXPECT_EQ(error.code(), ExtensionErrc::invalid_manifest);
        EXPECT_EQ(error.file(), file);
        ASSERT_EQ(error.diagnostics().size(), 1U);
        EXPECT_EQ(error.diagnostics()[0].line, 10U);
        EXPECT_EQ(error.diagnostics()[0].column, 1U);
        EXPECT_EQ(error.diagnostics()[0].message, "unknown key 'tittle' in [[contributes.menus]]");
        const std::u8string path = file.generic_u8string();
        EXPECT_EQ(std::string(error.what()),
                  std::string(path.begin(), path.end()) +
                      ":10:1: unknown key 'tittle' in [[contributes.menus]]");
    }
}

TEST(Manifest, ReportsAllProblemsInTheOrderOfTheFile) {
    const std::string text = with_header("homepage = \"me\"\n");
    std::string broken = text;
    broken.replace(broken.find("name = \"Case\""), 13, "name = 5");
    try {
        static_cast<void>(parse_manifest(broken, engine, "module/stresslab-extension.toml"));
        FAIL();
    } catch (const ManifestError& error) {
        EXPECT_EQ(std::string(error.what()),
                  "module/stresslab-extension.toml:3:8: 'name' in [extension] must be text, not "
                  "an integer\n"
                  "module/stresslab-extension.toml:8:1: unknown key 'homepage' in [extension]");
    }
}

TEST(Manifest, MissingFileIsAnIoError) {
    EXPECT_EXTENSION_ERROR(
        static_cast<void>(load_manifest(fixtures() / "no-such-module" / "x.toml", engine)),
        ExtensionErrc::io);
}

/// A module folder with a manifest and files of property schemas, in a folder whose name has
/// Cyrillic letters and spaces.
class SchemaFiles : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(folder / "schemas");
    }
    void TearDown() override { std::filesystem::remove_all(root); }

    void write(const std::filesystem::path& relative, std::string_view text) const {
        std::ofstream(folder / relative, std::ios::binary) << text;
    }

    /// The manifest of a module with one property schema whose `schema` is `value`.
    [[nodiscard]] std::filesystem::path module(std::string_view value) const {
        write(std::string(manifest_file_name),
              std::string(header_without_code) +
                  "[[contributes.propertySchemas]]\ntype = \"material\"\nschema = " +
                  std::string(value) + "\n");
        return folder / std::string(manifest_file_name);
    }

    /// The diagnostics of the manifest of `module(value)`, which must not parse.
    [[nodiscard]] std::vector<ManifestDiagnostic> problems(std::string_view value) const {
        try {
            static_cast<void>(load_manifest(module(value), engine));
        } catch (const ManifestError& error) {
            return error.diagnostics();
        }
        ADD_FAILURE() << "The manifest parsed with schema = " << value;
        return {};
    }

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / std::filesystem::path(u8"stresslab схемы модулей");
    const std::filesystem::path folder = root / std::filesystem::path(u8"мой модуль");
};

TEST_F(SchemaFiles, SchemaIsReadFromAFileOfTheModule) {
    write("schemas/material.json",
          R"({"type": "object", "required": ["E"],
              "properties": {"E": {"type": "number", "title": "%E%", "x-quantity": "pressure"}}})");
    const Manifest manifest = load_manifest(module("\"schemas/material.json\""), engine);
    ASSERT_EQ(manifest.contributes.property_schemas.size(), 1U);
    const nlohmann::json& schema = manifest.contributes.property_schemas[0].schema;
    EXPECT_EQ(schema.at("required"), nlohmann::json::array({"E"}));
    EXPECT_EQ(schema.at("properties").at("E").at("x-quantity"), "pressure");
}

TEST_F(SchemaFiles, ProblemsOfTheFileAreReportedAtTheKey) {
    write("schemas/broken.json", "{\"type\": \"object\",");
    write("schemas/list.json", "[1, 2]");
    write("schemas/stiff.json",
          R"({"type": "object", "properties": {"E": {"type": "number", "x-quantity": "stiff"}}})");
    struct Case {
        const char* value;
        std::string message;
    };
    const std::vector<Case> cases = {
        {"\"schemas/none.json\"",
         "cannot read the file 'schemas/none.json' of the property schema of type 'material'"},
        {"\"schemas/broken.json\"",
         "the file 'schemas/broken.json' of the property schema of type 'material' is not a JSON "
         "object"},
        {"\"schemas/list.json\"", "is not a JSON object"},
        {"\"schemas/stiff.json\"",
         "property schema of type 'material', property 'E': unknown quantity \"stiff\""},
        {"\"../outside.json\"", "invalid file '../outside.json' of the property schema"},
        {"\"C:/schemas/material.json\"", "invalid file 'C:/schemas/material.json'"},
        {"5",
         "'schema' in [[contributes.propertySchemas]] must be a table with a JSON Schema or "
         "the path of a JSON file, not an integer"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.value);
        const std::vector<ManifestDiagnostic> found = problems(c.value);
        ASSERT_EQ(found.size(), 1U);
        EXPECT_EQ(found[0].line, 8U);
        EXPECT_EQ(found[0].column, 10U);
        EXPECT_NE(found[0].message.find(c.message), std::string::npos) << found[0].message;
    }
}

struct ErrorCase {
    const char* name;
    std::string text;
    std::uint32_t line;
    std::uint32_t column;  // 0: not checked.
    const char* message;   // A part of the message.
};

std::string replace(std::string_view text, std::string_view from, std::string_view to) {
    std::string result(text);
    const std::size_t at = result.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    result.replace(at, from.size(), to);
    return result;
}

TEST(Manifest, ReportsErrorsWithTheirPosition) {
    const std::vector<ErrorCase> cases = {
        {"toml syntax", "[extension]\nid = \"test.case\n", 2, 0, "TOML syntax"},
        {"no [extension]", "[contributes]\n", 0, 0, "missing the [extension] table"},
        {"[extension] not a table", "extension = 5\n", 1, 13, "must be a table"},
        {"unknown key", with_header("homepage = \"me\"\n"), 8, 1,
         "unknown key 'homepage' in [extension]"},
        {"missing id", replace(header, "id = \"test.case\"\n", ""), 1, 1,
         "missing 'id' in [extension]"},
        {"uppercase id", replace(header, "test.case", "Test.Case"), 2, 6,
         "invalid module id 'Test.Case'"},
        {"one-part id", replace(header, "test.case", "test"), 2, 6, "invalid module id 'test'"},
        {"core id", replace(header, "test.case", "fem.core"), 2, 6,
         "'fem.core' is the platform itself"},
        {"name not text", replace(header, "\"Case\"", "5"), 3, 8,
         "'name' in [extension] must be text, not an integer"},
        {"empty name", replace(header, "\"Case\"", "\"\""), 3, 8, "'name' in [extension] is empty"},
        {"bad version", replace(header, "\"1.0.0\"", "\"1.0\""), 4, 11,
         "invalid 'version': '1.0' is not valid: expected major.minor.patch"},
        {"bad engine", replace(header, "\"^1.0\"", "\"^1.x\""), 5, 10, "invalid 'engine'"},
        {"unknown kind", replace(header, "\"python\"", "\"wasm\""), 6, 8,
         "unknown kind 'wasm'; expected native, python or process"},
        {"kind without entry", replace(header, "entry = \"case:activate\"\n", ""), 1, 1,
         "'kind' and 'entry' in [extension] go together"},
        {"python entry", replace(header, "\"case:activate\"", "\"case\""), 7, 9,
         "expected package.module:function"},
        {"native entry outside the folder",
         replace(replace(header, "\"python\"", "\"native\""), "\"case:activate\"",
                 "\"../bin/hello\""),
         7, 9, "invalid entry '../bin/hello' of a native module"},
        {"requires itself", with_header("requires = [\"test.case\"]\n"), 8, 13,
         "a module cannot require itself"},
        {"bad requirement range", with_header("requires = [\"test.other >=x\"]\n"), 8, 13,
         "invalid version range in the requirement 'test.other >=x'"},
        {"requirement twice", with_header("requires = [\"test.a\", \"test.a ^1\"]\n"), 8, 23,
         "duplicate requirement of module 'test.a' (first at line 8)"},
        {"requires the core", with_header("requires = [\"fem.core ^1.0\"]\n"), 8, 13,
         "'fem.core' is the platform"},
        {"requirement without id", with_header("requires = [\"Other\"]\n"), 8, 13,
         "must start with a module id"},
        {"unknown activation", with_header("activation = [\"onLoad\"]\n"), 8, 15,
         "unknown activation event 'onLoad'"},
        {"activation with a space", with_header("activation = [\"onCommand:a b\"]\n"), 8, 15,
         "unknown activation event"},
        {"activation without code",
         std::string(header_without_code) + "activation = [\"onStartup\"]\n", 6, 14,
         "activation events load code"},
        {"unknown contribution point", with_header("[[contributes.widgets]]\nid = \"x\"\n"), 8, 0,
         "unknown key 'widgets' in [contributes]"},
        {"point not an array", with_header("[contributes]\ncommands = \"x\"\n"), 9, 12,
         "'contributes.commands' must be an array of tables"},
        {"command without title", with_header("[[contributes.commands]]\nid = \"case.run\"\n"), 8,
         1, "missing 'title' in [[contributes.commands]]"},
        {"title not text", with_header("[[contributes.commands]]\nid = \"case.run\"\ntitle = 5\n"),
         10, 9, "'title' in [[contributes.commands]] must be text, not an integer"},
        {"one-part command id",
         with_header("[[contributes.commands]]\nid = \"run\"\ntitle = \"R\"\n"), 9, 6,
         "invalid id 'run' in [[contributes.commands]]"},
        {"command twice",
         with_header("[[contributes.commands]]\nid = \"case.run\"\ntitle = \"R\"\n"
                     "[[contributes.commands]]\nid = \"case.run\"\ntitle = \"R\"\n"),
         12, 6, "duplicate command id 'case.run' (first at line 9)"},
        {"unknown argument type",
         with_header("[[contributes.commands]]\nid = \"case.run\"\ntitle = \"R\"\n"
                     "args = [{ name = \"x\", type = \"float\" }]\n"),
         11, 30, "unknown argument type 'float'"},
        {"required argument with a default",
         with_header(
             "[[contributes.commands]]\nid = \"case.run\"\ntitle = \"R\"\n"
             "args = [{ name = \"x\", type = \"integer\", required = true, default = 1 }]\n"),
         11, 9, "required argument 'x' of command 'case.run' has a default"},
        {"default of another type",
         with_header("[[contributes.commands]]\nid = \"case.run\"\ntitle = \"R\"\n"
                     "args = [{ name = \"x\", type = \"integer\", default = \"a\" }]\n"),
         11, 9, "the default of argument 'x' of command 'case.run' is not of type integer"},
        {"argument named like a Python keyword",
         with_header("[[contributes.commands]]\nid = \"case.run\"\ntitle = \"R\"\n"
                     "args = [{ name = \"class\" }]\n"),
         11, 18, "invalid argument name 'class'"},
        {"date as a default",
         with_header("[[contributes.commands]]\nid = \"case.run\"\ntitle = \"R\"\n"
                     "args = [{ name = \"x\", default = 1979-05-27 }]\n"),
         11, 33, "dates and times are not allowed"},
        {"commands without code",
         std::string(header_without_code) +
             "[[contributes.commands]]\nid = \"case.run\"\ntitle = \"R\"\n",
         6, 1, "commands need code"},
        {"bad when",
         with_header("[[contributes.menus]]\nlocation = \"tools\"\ncommand = \"case.run\"\n"
                     "when = \"a &&\"\n"),
         11, 8, "invalid 'when' in [[contributes.menus]]: column 5:"},
        {"bad enablement",
         with_header("[[contributes.commands]]\nid = \"case.run\"\ntitle = \"R\"\n"
                     "enablement = \"!\"\n"),
         11, 14, "invalid 'enablement' in [[contributes.commands]]: column 2:"},
        {"submenu id with a slash",
         with_header("[[contributes.submenus]]\nid = \"a/b\"\ntitle = \"A\"\n"), 9, 6,
         "one part of a menu location"},
        {"submenu id twice",
         with_header("[[contributes.submenus]]\nid = \"a\"\ntitle = \"A\"\n"
                     "[[contributes.submenus]]\nid = \"a\"\ntitle = \"B\"\n"),
         12, 6, "duplicate submenu id 'a' (first at line 9)"},
        {"bad stage slot",
         with_header("[[contributes.stages]]\nid = \"case.stage\"\ntitle = \"S\"\n"
                     "slot = \"inside:mesh\"\n"),
         11, 8, "invalid slot 'inside:mesh'"},
        {"two activity bar entries",
         with_header("[[contributes.activityBar]]\nid = \"case.a\"\ntitle = \"A\"\n"
                     "icon = \"a.svg\"\n[[contributes.activityBar]]\nid = \"case.b\"\n"
                     "title = \"B\"\nicon = \"b.svg\"\n"),
         12, 1, "a module has at most one entry in [[contributes.activityBar]]"},
        {"mesher parameters without a schema",
         with_header("[[contributes.meshers]]\nid = \"mesh\"\nlabel = \"M\"\n"
                     "parameters = \"mesh_options\"\n"),
         11, 14, "the parameters 'mesh_options' of a mesher need a"},
        {"unknown modifier",
         with_header("[[contributes.keybindings]]\ncommand = \"case.run\"\nkey = \"Hyper+R\"\n"),
         10, 7, "'Hyper' is not a modifier"},
        {"modifiers only",
         with_header("[[contributes.keybindings]]\ncommand = \"case.run\"\nkey = \"Mod+Shift\"\n"),
         10, 7, "no key after the modifiers"},
        {"file extension without a dot",
         with_header("[[contributes.fileFormats]]\nid = \"res\"\nextensions = [\"res\"]\n"
                     "role = \"results-import\"\n"),
         10, 15, "invalid file extension"},
        {"unknown file format role",
         with_header("[[contributes.fileFormats]]\nid = \"res\"\nextensions = [\".res\"]\n"
                     "role = \"results-view\"\n"),
         11, 8, "unknown role 'results-view'"},
        {"icon outside the folder",
         with_header("[[contributes.icons]]\nid = \"case.icon\"\npath = \"../icon.svg\"\n"), 10, 8,
         "invalid path '../icon.svg'"},
        {"absolute icon path",
         with_header("[[contributes.icons]]\nid = \"case.icon\"\npath = \"/etc/icon.svg\"\n"), 10,
         8, "invalid path '/etc/icon.svg'"},
        {"unknown setting type",
         with_header("[contributes.settings]\n\"case.x\" = { type = \"float\" }\n"), 9, 21,
         "unknown setting type 'float'"},
        {"setting default of another type",
         with_header(
             "[contributes.settings]\n\"case.x\" = { type = \"integer\", default = \"a\" }\n"),
         9, 42, "the default of setting 'case.x' must be integer, not text"},
        {"setting default not in enum",
         with_header(
             "[contributes.settings]\n"
             "\"case.x\" = { type = \"string\", default = \"c\", enum = [\"a\", \"b\"] }\n"),
         9, 41, "is not one of the values of 'enum'"},
        {"unknown setting scope",
         with_header(
             "[contributes.settings]\n\"case.x\" = { type = \"string\", scope = \"machine\" }\n"),
         9, 39, "unknown setting scope 'machine'; expected project or user"},
        {"setting title not text",
         with_header("[contributes.settings]\n\"case.x\" = { type = \"string\", title = 5 }\n"), 9,
         39, "'title' in setting 'case.x' must be text"},
        {"unknown quantity of a result indicator",
         with_header("[[contributes.resultIndicators]]\nid = \"case.mass\"\ntitle = \"M\"\n"
                     "quantity = \"weight\"\n"),
         11, 12, "unknown quantity 'weight' in [[contributes.resultIndicators]]"},
        {"unknown quantity in a property schema",
         with_header("[[contributes.propertySchemas]]\ntype = \"material\"\nschema = { type = "
                     "\"object\", properties = { E = { type = \"number\", x-quantity = "
                     "\"stiffness\" } } }\n"),
         10, 10,
         "property schema of type 'material', property 'E': unknown quantity \"stiffness\""},
        {"property schema of no object",
         with_header("[[contributes.propertySchemas]]\ntype = \"material\"\nschema = { type = "
                     "\"string\" }\n"),
         10, 10, "property schema of type 'material': a property schema must have type"},
        {"unknown setting field",
         with_header(
             "[contributes.settings]\n\"case.x\" = { type = \"string\", markdown = \"x\" }\n"),
         9, 31, "unknown key 'markdown' in setting 'case.x'"},
        {"bad context key name",
         with_header("[[contributes.contextKeys]]\nname = \"1abc\"\ntype = \"boolean\"\n"), 9, 8,
         "invalid name '1abc'"},
        {"context key default of another type",
         with_header("[[contributes.contextKeys]]\nname = \"case.list\"\ntype = \"list\"\n"
                     "default = [1, 2]\n"),
         11, 11, "the default of context key 'case.list' must be an array of text, not an array"},
        {"language twice",
         with_header("[[contributes.localization]]\nlanguage = \"ru\"\npath = \"ru.json\"\n"
                     "[[contributes.localization]]\nlanguage = \"ru\"\npath = \"ru2.json\"\n"),
         12, 12, "duplicate language 'ru' (first at line 9)"},
        {"solver id with a dot",
         with_header("[[contributes.solvers]]\nid = \"my.solver\"\nlabel = \"S\"\n"), 9, 6,
         "invalid id 'my.solver'"},
    };
    for (const ErrorCase& c : cases) {
        SCOPED_TRACE(c.name);
        const std::vector<ManifestDiagnostic> found = diagnostics(c.text);
        ASSERT_EQ(found.size(), 1U)
            << ::testing::PrintToString(found.size()) << " problems:\n"
            << [&] {
                   std::string all;
                   for (const ManifestDiagnostic& d : found) {
                       all += std::to_string(d.line) + ":" + std::to_string(d.column) + " " +
                              d.message + "\n";
                   }
                   return all;
               }();
        EXPECT_EQ(found[0].line, c.line) << found[0].message;
        if (c.column != 0) {
            EXPECT_EQ(found[0].column, c.column) << found[0].message;
        }
        EXPECT_NE(found[0].message.find(c.message), std::string::npos) << found[0].message;
    }
}

TEST(ExtensionId, IsLowercaseWithTwoOrMoreParts) {
    using fem::extension::is_valid_extension_id;
    for (const char* id : {"fem.calc", "fem.mesher.gmsh", "fem.format.ansys-cdb", "a.b_2"}) {
        EXPECT_TRUE(is_valid_extension_id(id)) << id;
    }
    for (const char* id :
         {"", "fem", "Fem.x", "fem..x", "fem.x.", "fem.2x", "fem.x y", "fem.core"}) {
        EXPECT_FALSE(is_valid_extension_id(id)) << id;
    }
}

TEST(ActivationEvent, ParsesAndPrintsTheFormsOfManifests) {
    for (const char* text :
         {"onStartup", "onCommand:solver.run", "onCommand:solver.*", "onOpenFile:*.res",
          "onOpenFile:модель?.dat", "onSolverRequested:my-solver"}) {
        const auto event = ActivationEvent::parse(text);
        ASSERT_TRUE(event.has_value()) << text;
        EXPECT_EQ(event->to_string(), text);
    }
    for (const char* text : {"", "onstartup", "onStartup:x", "onCommand:", "onCommand:a b",
                             "onOpenFile:dir/*.res", "onSolverRequested:a.b", "onLoad"}) {
        EXPECT_FALSE(ActivationEvent::parse(text).has_value()) << text;
    }
}

TEST(ActivationEvent, PatternsMatchFiredEvents) {
    const auto pattern = [](const char* text) { return *ActivationEvent::parse(text); };
    EXPECT_TRUE(pattern("onStartup").matches(ActivationEvent::startup()));
    EXPECT_FALSE(pattern("onStartup").matches(ActivationEvent::command("a.b")));
    EXPECT_TRUE(pattern("onCommand:solver.*").matches(ActivationEvent::command("solver.run")));
    EXPECT_TRUE(pattern("onCommand:*.run").matches(ActivationEvent::command("solver.run")));
    EXPECT_FALSE(pattern("onCommand:solver.*").matches(ActivationEvent::command("post.run")));
    EXPECT_FALSE(pattern("onCommand:Solver.run").matches(ActivationEvent::command("solver.run")));
    EXPECT_TRUE(pattern("onCommand:s?lver.r*n").matches(ActivationEvent::command("solver.run")));
    EXPECT_TRUE(
        pattern("onSolverRequested:demo").matches(ActivationEvent::solver_requested("demo")));
    EXPECT_FALSE(
        pattern("onSolverRequested:demo").matches(ActivationEvent::solver_requested("demo2")));

    // File names: without the folders and without ASCII case; '?' is one character.
    const std::filesystem::path file =
        std::filesystem::path(u8"папка с пробелами") / std::filesystem::path(u8"Модель1.RES");
    EXPECT_EQ(ActivationEvent::open_file(file).value, utf8(u8"Модель1.RES"));
    EXPECT_TRUE(pattern("onOpenFile:*.res").matches(ActivationEvent::open_file(file)));
    const ActivationEvent any_six_letters{ActivationEvent::Trigger::open_file, utf8(u8"??????1.*")};
    EXPECT_TRUE(any_six_letters.matches(ActivationEvent::open_file(file)));
    EXPECT_FALSE(pattern("onOpenFile:*.dat").matches(ActivationEvent::open_file(file)));
    EXPECT_FALSE(pattern("onOpenFile:*.res").matches(ActivationEvent::command("x.res")));
}

}  // namespace
