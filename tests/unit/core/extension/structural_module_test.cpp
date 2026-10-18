// The built-in module extensions/fem.model.structural (task 3.1,
// docs/adr/0023-structural-model-and-results-ir.md): the object types of a structural model and
// of its results, their forms and their translations. That objects of these types round-trip
// through a project is tested by tests/unit/storage/structural_round_trip_test.cpp; the samples
// of properties both use are in tests/data/structural/samples.json.

#include "fem/core/bindings.h"
#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/extension/contributions.h"
#include "fem/core/extension/manager.h"
#include "fem/core/form/form.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/document.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::extension::ExtensionManager;
using fem::extension::ExtensionSource;

constexpr const char* id = "fem.model.structural";
const fs::path folder = fs::path(FEM_BUILTIN_EXTENSIONS_DIR) / id;

/// Every object type of the module, in the order of its manifest.
const std::vector<std::string> types = {
    "material",           "solid_section",     "shell_section", "beam_section", "static_step",
    "boundary_condition", "concentrated_load", "pressure",      "gravity",      "output_request",
    "result_step",        "result_frame",      "result_field"};

std::string read(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

nlohmann::json read_json(const fs::path& file) {
    return nlohmann::json::parse(read(file));
}

std::set<std::string> catalog_keys(std::string_view language) {
    const nlohmann::json catalog = read_json(folder / "l10n" / (std::string(language) + ".json"));
    std::set<std::string> keys;
    for (const auto& [key, value] : catalog.at("strings").items()) {
        keys.insert(key);
    }
    return keys;
}

/// "%key%" references in a text.
void collect_references(const std::string& text, std::set<std::string>& out) {
    const std::regex reference("%([A-Za-z0-9_.-]+)%");
    for (std::sregex_iterator it(text.begin(), text.end(), reference), end; it != end; ++it) {
        out.insert((*it)[1].str());
    }
}

const nlohmann::json& samples() {
    static const nlohmann::json data =
        read_json(fs::path(FEM_TEST_DATA_DIR) / "structural" / "samples.json");
    return data;
}

/// Problems as (pointer, code) pairs, sorted.
std::vector<std::pair<std::string, std::string>> codes(
    const std::vector<fem::form::Problem>& problems) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const fem::form::Problem& problem : problems) {
        result.emplace_back(problem.pointer, std::string(fem::form::to_string(problem.code)));
    }
    std::sort(result.begin(), result.end());
    return result;
}

/// The module installed from the folder the application ships and enabled.
class StructuralModule : public ::testing::Test {
protected:
    StructuralModule() {
        (void)manager.install(folder, ExtensionSource::builtin);
        (void)manager.enable(id);
    }

    [[nodiscard]] const fem::extension::Contributions& contributions() const {
        return *manager.registry().find(id);
    }

    /// The form of a type; fails the test if its schema has mistakes.
    [[nodiscard]] fem::form::FormSpec form(const std::string& type) const {
        for (const auto& schema : contributions().property_schemas) {
            if (schema.type == type) {
                std::vector<fem::form::SchemaError> errors;
                fem::form::FormSpec spec = fem::form::parse(schema.schema, &errors);
                for (const auto& error : errors) {
                    ADD_FAILURE() << type << " " << error.pointer << ": " << error.message;
                }
                return spec;
            }
        }
        ADD_FAILURE() << "no property schema of " << type;
        return {};
    }

    fem::model::Document document;
    fem::command::CommandBus bus{document};
    fem::context::ContextKeys keys;
    ExtensionManager manager{document, bus, keys};
};

TEST_F(StructuralModule, IsAModuleWithoutCodeThatDefinesTheTypes) {
    const auto& info = *manager.find(id);
    EXPECT_FALSE(info.manifest->has_code());
    EXPECT_TRUE(document.is_owner_enabled(id));

    const auto& contributes = contributions();
    EXPECT_TRUE(contributes.commands.empty());
    std::vector<std::string> nodes;
    for (const auto& node : contributes.tree_node_types) {
        nodes.push_back(node.type);
    }
    EXPECT_EQ(nodes, types);
    std::vector<std::string> schemas;
    for (const auto& schema : contributes.property_schemas) {
        schemas.push_back(schema.type);
        EXPECT_EQ(schema.version, 1U) << schema.type;
    }
    EXPECT_EQ(schemas, types);
}

TEST_F(StructuralModule, EveryFormHasNoMistakesAndTitledFields) {
    for (const std::string& type : types) {
        SCOPED_TRACE(type);
        const fem::form::FormSpec spec = form(type);
        EXPECT_TRUE(spec.closed);
        for (const fem::form::Field* field : fem::form::all_fields(spec)) {
            EXPECT_EQ(field->title.front(), '%') << field->pointer;
            if (type != "result_field" || field->pointer != "components") {
                EXPECT_NE(field->kind, fem::form::FieldKind::raw)
                    << field->pointer << " is shown as JSON text";
            }
            if (!field->default_value.is_null()) {
                EXPECT_FALSE(fem::form::check_value(*field, field->default_value).has_value())
                    << "the default of " << field->pointer << " does not fit it";
            }
        }
    }
}

TEST_F(StructuralModule, OnlyTheComponentsOfAFieldOfResultsAreShownAsText) {
    // An array of text, read-only like everything of a result; forms show it as JSON text.
    const fem::form::FormSpec spec = form("result_field");
    const fem::form::Field* components = fem::form::find_field(spec, "components");
    ASSERT_NE(components, nullptr);
    EXPECT_EQ(components->kind, fem::form::FieldKind::raw);
    for (const fem::form::Field* field : fem::form::all_fields(spec)) {
        EXPECT_TRUE(field->read_only) << field->pointer;
    }
}

TEST_F(StructuralModule, ValuesWithUnitsHaveTheirQuantities) {
    const std::map<std::string, std::string> expected = {
        {"material/youngsModulus", "pressure"}, {"material/density", "density"},
        {"shell_section/thickness", "length"},  {"beam_section/radius", "length"},
        {"boundary_condition/ux", "length"},    {"boundary_condition/rz", "angle"},
        {"concentrated_load/fy", "force"},      {"concentrated_load/mz", "moment"},
        {"pressure/magnitude", "pressure"},     {"gravity/acceleration", "acceleration"},
    };
    for (const auto& [where, quantity] : expected) {
        const std::size_t slash = where.find('/');
        const fem::form::FormSpec spec = form(where.substr(0, slash));
        const fem::form::Field* field = fem::form::find_field(spec, where.substr(slash + 1));
        ASSERT_NE(field, nullptr) << where;
        ASSERT_NE(field->quantity, nullptr) << where;
        EXPECT_EQ(field->quantity->id, quantity) << where;
    }
    const fem::form::FormSpec material = form("material");
    EXPECT_EQ(fem::form::find_field(material, "poissonsRatio")->quantity, nullptr);
}

TEST_F(StructuralModule, ValidSamplesFitTheirForms) {
    const nlohmann::json& valid = samples().at("valid");
    for (const std::string& type : types) {
        SCOPED_TRACE(type);
        ASSERT_TRUE(valid.contains(type)) << "no sample";
        const fem::form::FormSpec spec = form(type);
        for (const nlohmann::json& properties : valid.at(type)) {
            EXPECT_EQ(codes(fem::form::check(spec, properties)),
                      (std::vector<std::pair<std::string, std::string>>{}))
                << properties.dump();
        }
    }
}

TEST_F(StructuralModule, InvalidSamplesHaveTheirProblems) {
    for (const nlohmann::json& sample : samples().at("invalid")) {
        const std::string type = sample.at("type").get<std::string>();
        SCOPED_TRACE(type + " " + sample.at("properties").dump());
        std::vector<std::pair<std::string, std::string>> expected;
        for (const nlohmann::json& problem : sample.at("problems")) {
            expected.emplace_back(problem.at(0).get<std::string>(),
                                  problem.at(1).get<std::string>());
        }
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(codes(fem::form::check(form(type), sample.at("properties"))), expected);
    }
}

TEST_F(StructuralModule, EveryTextIsTranslatedToEnglishAndRussian) {
    const std::set<std::string> english = catalog_keys("en");
    EXPECT_EQ(catalog_keys("ru"), english);

    std::set<std::string> referenced;
    collect_references(read(folder / "stresslab-extension.toml"), referenced);
    for (const fs::directory_entry& entry : fs::directory_iterator(folder / "schemas")) {
        collect_references(read(entry.path()), referenced);
    }
    for (const std::string& key : referenced) {
        EXPECT_TRUE(english.contains(key)) << "%" << key << "% has no text";
    }
    for (const std::string& key : english) {
        EXPECT_TRUE(referenced.contains(key)) << key << " is not used";
    }

    fem::l10n::Localization localization;
    const fem::Subscription catalogs = fem::bind_catalogs(manager, localization);
    localization.set_language("ru");
    EXPECT_EQ(localization.manifest_text(id, "%node.material%"), "Материал");
    localization.set_language("en");
    EXPECT_EQ(localization.manifest_text(id, "%node.boundary_condition%"), "Boundary condition");
}

TEST_F(StructuralModule, ObjectsOfTheModuleBecomeOpaqueWhenItIsDisabled) {
    // What the model of a user holds stays when the module is off (CLAUDE.md, rule 5).
    fem::model::NewObject material;
    material.type = "material";
    material.owner = id;
    material.name = "steel";
    material.properties = samples().at("valid").at("material").at(0);
    bus.register_command({.id = "test.add", .owner = "test.harness"},
                         [&](fem::command::CommandContext& context) {
                             (void)context.editor().create(material);
                             return fem::command::CommandResult{};
                         });
    bus.execute("test.add");
    const fem::model::ObjectId steel = document.find_by_type("material").at(0);
    (void)manager.disable(id);
    EXPECT_TRUE(document.is_opaque(steel));
    EXPECT_EQ(document.get(steel).properties(), material.properties);
    (void)manager.enable(id);
    EXPECT_FALSE(document.is_opaque(steel));
}

}  // namespace
