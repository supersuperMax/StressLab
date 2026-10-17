// Diffs of two states of a project in the terms of the model (task 2.4): what changed in objects
// and fields, said with the titles of the modules, objects of disabled modules as raw JSON.

#include "fem/storage/diff.h"

#include "fem/core/extension/contributions.h"
#include "fem/core/extension/registry.h"
#include "fem/core/io/files.h"
#include "fem/storage/error.h"
#include "fem/storage/json.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace fem::storage {
namespace {

using namespace testing;
using model::ObjectId;
using Json = nlohmann::json;

constexpr const char* structural = "test.structural";

fs::path data_dir() {
    return fs::path(FEM_TEST_DATA_DIR) / "history";
}

std::string read_text(const fs::path& file) {
    std::optional<std::string> text = io::read_file(file);
    EXPECT_TRUE(text.has_value()) << file;
    std::string result = text.value_or("");
    std::erase(result, '\r');  // A checkout may have made CRLF.
    return result;
}

/// The files of a project folder of the test data.
ProjectFiles fixture(std::string_view name) {
    const fs::path folder = data_dir() / name;
    ProjectFiles files;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            const std::u8string path = entry.path().lexically_relative(folder).generic_u8string();
            files.emplace(std::string(path.begin(), path.end()), read_text(entry.path()));
        }
    }
    return files;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        lines.push_back(text.substr(start, end - start));
        start = end == std::string::npos ? text.size() : end + 1;
    }
    return lines;
}

/// A file of the files as JSON, changed by `edit`, written back.
template <class Edit>
void edit_file(ProjectFiles& files, const std::string& path, Edit edit) {
    Json value = parse_json(files.at(path), path);
    edit(value);
    files[path] = to_canonical_json(value);
}

const std::string section_file = "model/ext/test.structural/section/4.json";

/// The contributions of the module of the fixtures: titles of its object types and the forms of
/// its sections, materials and loads.
std::shared_ptr<extension::Contributions> structural_module() {
    auto module = std::make_shared<extension::Contributions>();
    module->owner = structural;
    for (const char* type : {"material", "section", "load"}) {
        module->tree_node_types.push_back({.owner = structural,
                                           .type = type,
                                           .title = std::string("%type.") + type + "%",
                                           .icon = {}});
    }
    const auto schema = [&](const std::string& type, Json properties) {
        module->property_schemas.push_back(
            {.owner = structural,
             .type = type,
             .schema = {{"type", "object"}, {"properties", std::move(properties)}}});
    };
    schema("material",
           {{"E", {{"type", "number"}, {"title", "%material.E%"}, {"x-quantity", "pressure"}}},
            {"nu", {{"type", "number"}, {"title", "%material.nu%"}}}});
    schema("section",
           {{"thickness",
             {{"type", "number"}, {"title", "%section.thickness%"}, {"x-quantity", "length"}}},
            {"kind",
             {{"title", "%section.kind%"},
              {"enum", {"shell", "composite"}},
              {"x-enumTitles", {"%section.kind.shell%", "%section.kind.composite%"}}}},
            {"integration",
             {{"type", "object"},
              {"title", "%section.integration%"},
              {"properties",
               {{"points", {{"type", "integer"}, {"title", "%section.points%"}}},
                {"rule", {{"type", "string"}}}}}}},
            {"offsets", {{"type", "array"}, {"title", "%section.offsets%"}}},
            {"plies", {{"type", "integer"}, {"title", "%section.plies%"}}},
            {"note", {{"type", "string"}, {"title", "%section.note%"}}}});
    schema("load",
           {{"magnitude",
             {{"type", "number"}, {"title", "%load.magnitude%"}, {"x-quantity", "pressure"}}}});
    return module;
}

class StorageDiff : public ::testing::Test {
protected:
    StorageDiff() {
        registry.add(structural_module());
        for (const char* language : {"en", "ru"}) {
            EXPECT_TRUE(localization.add_catalog_file(
                structural, language, data_dir() / "l10n" / (std::string(language) + ".json")));
        }
    }

    std::vector<std::string> text(const ModelDiff& diff, std::string_view language,
                                  bool with_registry = true) {
        localization.set_language(language);
        return describe(
            diff, {.localization = localization, .registry = with_registry ? &registry : nullptr});
    }

    extension::ContributionRegistry registry;
    l10n::Localization localization;
    ProjectFiles main = fixture("wing-main.slproj");
    ProjectFiles composite = fixture("wing-composite.slproj");
};

}  // namespace

// --- The structure -------------------------------------------------------------------------------

TEST_F(StorageDiff, SameFilesHaveNoDiff) {
    const ModelDiff diff = diff_files(main, main);
    EXPECT_TRUE(diff.empty());
    EXPECT_TRUE(diff.objects.empty());
    EXPECT_EQ(to_json(diff), Json::parse(R"({"objects": []})"));
    EXPECT_TRUE(describe(diff, {.localization = localization}).empty());
}

TEST_F(StorageDiff, WingVariantsMatchTheGoldenJson) {
    const ModelDiff diff = diff_files(main, composite);
    const Json expected =
        parse_json(read_text(data_dir() / "expected" / "wing.diff.json"), "wing.diff.json");
    EXPECT_EQ(to_canonical_json(to_json(diff)), to_canonical_json(expected));
}

TEST_F(StorageDiff, KindsOfChangesById) {
    const ModelDiff diff = diff_files(main, composite);
    std::vector<std::string> kinds;
    for (const ObjectChange& change : diff.objects) {
        kinds.push_back(std::to_string(change.id.value()) + " " +
                        std::string(to_string(change.kind)));
    }
    EXPECT_EQ(kinds, (std::vector<std::string>{"1 changed", "2 changed", "3 changed", "4 changed",
                                               "5 changed", "6 changed", "7 removed", "8 added",
                                               "9 added"}));
    const ObjectChange* section = diff.find(ObjectId(4));
    ASSERT_NE(section, nullptr);
    EXPECT_EQ(section->dependencies_added, std::vector<ObjectId>{ObjectId(8)});
    EXPECT_EQ(section->dependencies_removed, std::vector<ObjectId>{ObjectId(3)});
    EXPECT_FALSE(section->name);
    EXPECT_FALSE(section->data);
    EXPECT_TRUE(diff.find(ObjectId(1))->data);
    EXPECT_EQ(diff.find(ObjectId(7))->current().object.init.name, "cruise");
    EXPECT_FALSE(diff.find(ObjectId(7))->after.has_value());
    EXPECT_FALSE(diff.find(ObjectId(8))->before.has_value());
    EXPECT_EQ(diff.find(ObjectId(42)), nullptr);
    // The objects the changes refer to are named.
    EXPECT_EQ(diff.referenced.at(ObjectId(3)).name, "Д16Т");
    EXPECT_EQ(diff.referenced.at(ObjectId(8)).name, "КМ-1");
}

TEST_F(StorageDiff, ReverseDiffSwapsAddedAndRemoved) {
    const ModelDiff diff = diff_files(composite, main);
    EXPECT_EQ(diff.find(ObjectId(7))->kind, ChangeKind::added);
    EXPECT_EQ(diff.find(ObjectId(9))->kind, ChangeKind::removed);
    const ObjectChange* section = diff.find(ObjectId(4));
    EXPECT_EQ(section->dependencies_added, std::vector<ObjectId>{ObjectId(3)});
    EXPECT_EQ(section->properties.size(), 6U);
}

TEST_F(StorageDiff, PropertiesAreComparedByLeaves) {
    edit_file(main, section_file, [](Json& value) {
        value["properties"] = {{"a/b", 1}, {"c~d", {{"e", 1}}}, {"same", {1, 2}}, {"n", 1}};
    });
    edit_file(composite, section_file, [](Json& value) {
        value["properties"] = {
            {"a/b", 2}, {"c~d", {{"e", 1}, {"f", nullptr}}}, {"same", {1, 2}}, {"n", 1.0}};
    });
    const ModelDiff diff = diff_files(main, composite);
    const ObjectChange* section = diff.find(ObjectId(4));
    ASSERT_NE(section, nullptr);
    std::vector<std::string> changes;
    for (const PropertyChange& property : section->properties) {
        changes.push_back(property.pointer + " " +
                          (property.before ? property.before->dump() : "-") + " " +
                          (property.after ? property.after->dump() : "-"));
    }
    // '/' and '~' in keys are escaped as in JSON Pointer; null is a value, unlike no value; an
    // integer and a float with the same value differ, as they do in the files.
    EXPECT_EQ(changes, (std::vector<std::string>{"a~1b 1 2", "c~0d/f - null", "n 1 1.0"}));
}

TEST_F(StorageDiff, NamesParentsSchemasAndUnits) {
    edit_file(composite, section_file, [](Json& value) {
        value["parent"] = 1;
        value["schemaVersion"] = 2;
        value["name"] = "";
    });
    edit_file(composite, "project.json", [](Json& value) { value["units"]["length"] = "m"; });
    const ModelDiff diff = diff_files(main, composite);
    EXPECT_TRUE(diff.units_changed());
    EXPECT_EQ(diff.units_after.length, model::LengthUnit::meter);
    const ObjectChange* section = diff.find(ObjectId(4));
    EXPECT_TRUE(section->name);
    EXPECT_TRUE(section->parent);
    EXPECT_TRUE(section->schema_version);
    EXPECT_EQ(diff.referenced.at(ObjectId(1)).type, "mesh");

    const Json json = to_json(diff);
    EXPECT_EQ(json["units"], Json::parse(R"({"after": "m-t-s-degC", "before": "mm-t-s-degC"})"));
    EXPECT_EQ(json["objects"][3]["parent"], Json::parse(R"({"after": 1, "before": null})"));
    EXPECT_EQ(json["objects"][3]["schemaVersion"], Json::parse(R"({"after": 2, "before": 1})"));

    const std::vector<std::string> lines = text(diff, "en");
    ASSERT_GE(lines.size(), 5U);
    EXPECT_EQ(lines[0], "Units: mm-t-s-degC → m-t-s-degC");
    EXPECT_EQ(lines[4].substr(0, lines[4].find(", Integration")),
              "Section #4: name skin-upper → —, parent — → Mesh main, schema version 1 → 2");
    // The units of the model changed: each side is shown with its own.
    EXPECT_NE(lines[4].find("Thickness 2 mm → 3 m"), std::string::npos) << lines[4];
}

TEST_F(StorageDiff, ModuleVersionsAndNextIdAreNotChanges) {
    ProjectFiles later = main;
    edit_file(later, "project.json", [](Json& value) {
        value["modules"]["test.structural"] = "2.0.0";
        value["nextObjectId"] = 100;
    });
    EXPECT_TRUE(diff_files(main, later).empty());
}

TEST_F(StorageDiff, AnotherBlockWithTheSameCountsIsAChange) {
    ProjectFiles later = main;
    edit_file(later, "refs/blobs.json",
              [](Json& value) { value["1"] = "sha256:" + std::string(64, 'e'); });
    const ModelDiff diff = diff_files(main, later);
    ASSERT_EQ(diff.objects.size(), 1U);
    EXPECT_TRUE(diff.objects[0].data);
    EXPECT_EQ(text(diff, "en"), std::vector<std::string>{
                                    "Mesh main: rebuilt (the same number of nodes and elements)"});
    EXPECT_EQ(text(diff, "ru"),
              std::vector<std::string>{"Сетка main: перестроена (число узлов и элементов то же)"});
}

TEST_F(StorageDiff, BrokenFilesAreReported) {
    ProjectFiles broken = composite;
    broken[section_file] = "{";
    try {
        (void)diff_files(main, broken);
        ADD_FAILURE() << "expected ProjectError";
    } catch (const ProjectError& error) {
        EXPECT_EQ(error.code(), ProjectErrc::invalid_file);
        EXPECT_EQ(error.file(), section_file);
    }
    ProjectFiles no_project = composite;
    no_project.erase("project.json");
    EXPECT_THROW((void)diff_files(main, no_project), ProjectError);
}

// --- In words ------------------------------------------------------------------------------------

TEST_F(StorageDiff, WingVariantsInEnglish) {
    EXPECT_EQ(text(diff_files(main, composite), "en"),
              lines_of(read_text(data_dir() / "expected" / "wing.en.txt")));
}

TEST_F(StorageDiff, WingVariantsInRussian) {
    EXPECT_EQ(text(diff_files(main, composite), "ru"),
              lines_of(read_text(data_dir() / "expected" / "wing.ru.txt")));
}

TEST_F(StorageDiff, WithoutRegistryObjectsAreNamedByTypes) {
    const std::vector<std::string> lines = text(diff_files(main, composite), "en", false);
    ASSERT_EQ(lines.size(), 9U);
    EXPECT_EQ(lines[0], "Mesh main: rebuilt (48312 → 61007 elements)");
    EXPECT_EQ(lines[2], "material Д16Т: E 72000 → 71000");
    // Without a registry nothing is known to be disabled.
    EXPECT_EQ(lines[5], "damper d1: c 10 → 12");
    EXPECT_EQ(lines[8], "Added: load gust-2");
}

TEST_F(StorageDiff, ObjectsOfDisabledModulesAreRawJson) {
    // fem.dynamics is not in the registry; once test.structural is gone too, so are its titles.
    extension::ContributionRegistry empty;
    localization.set_language("en");
    const std::vector<std::string> lines =
        describe(diff_files(main, composite), {.localization = localization, .registry = &empty});
    EXPECT_EQ(
        lines[5],
        R"(damper d1 (module fem.dynamics is not enabled): {"properties":{"c":10.0}} → {"properties":{"c":12.0}})");
    EXPECT_EQ(
        lines[6],
        R"(Removed: load cruise (module test.structural is not enabled) {"dependencies":[2],"name":"cruise","parent":null,"properties":{"magnitude":0.02},"schemaVersion":1})");
    EXPECT_EQ(
        lines[8],
        R"(Added: load gust-2 (module test.structural is not enabled) {"dependencies":[2],"name":"gust-2","parent":null,"properties":{"magnitude":0.08},"schemaVersion":1})");
    // Objects of the core are never raw.
    EXPECT_EQ(lines[0], "Mesh main: rebuilt (48312 → 61007 elements)");
}

TEST_F(StorageDiff, ValuesInWords) {
    edit_file(main, section_file, [](Json& value) {
        value["properties"] = {{"flag", true}, {"kind", "plate"}, {"text", "a\nb"}};
    });
    edit_file(composite, section_file, [](Json& value) {
        value["properties"] = {{"flag", false}, {"kind", "shell"}, {"text", ""}};
        value["dependencies"] = {3};
    });
    const std::vector<std::string> en = text(diff_files(main, composite), "en");
    EXPECT_EQ(en[3],
              R"(Section skin-upper: flag yes → no, Kind "plate" → Shell, text "a\nb" → "")");
    const std::vector<std::string> ru = text(diff_files(main, composite), "ru");
    EXPECT_EQ(ru[3],
              R"(Сечение skin-upper: flag да → нет, Тип "plate" → Оболочка, text "a\nb" → "")");
}

TEST_F(StorageDiff, SetsAndPluralsInRussian) {
    ProjectFiles later = main;
    edit_file(later, "model/node_set/2.json", [](Json& value) { value["data"]["size"] = 41; });
    edit_file(later, "refs/blobs.json",
              [](Json& value) { value["2"] = "sha256:" + std::string(64, 'f'); });
    edit_file(later, "model/mesh/1.json", [](Json& value) { value["data"]["nodes"] = 9505; });
    edit_file(later, "refs/blobs.json",
              [](Json& value) { value["1"] = "sha256:" + std::string(64, '0'); });
    const ModelDiff diff = diff_files(main, later);
    EXPECT_EQ(text(diff, "ru"),
              (std::vector<std::string>{"Сетка main: перестроена (9504 → 9505 узлов)",
                                        "Набор узлов root: 40 → 41 узел"}));
    EXPECT_EQ(text(diff, "en"), (std::vector<std::string>{"Mesh main: rebuilt (9504 → 9505 nodes)",
                                                          "Node set root: 40 → 41 nodes"}));
}

}  // namespace fem::storage
