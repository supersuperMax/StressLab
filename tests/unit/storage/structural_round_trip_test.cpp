// CLAUDE.md rule 6 for the object types of fem.model.structural (task 3.1,
// docs/adr/0023-structural-model-and-results-ir.md): a model with an object of every type of a
// structural model, and results with fields of every position, give the same bytes when saved,
// loaded and saved again, also while the module is disabled. The properties are the samples of
// tests/data/structural/samples.json, which tests/unit/core/extension/structural_module_test.cpp
// checks against the forms of the module.

#include "fem/core/model/array_data.h"
#include "fem/core/model/object.h"
#include "fem/storage/project.h"

#include "project_fixture.h"
#include "storage_test_support.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fem::storage {
namespace {

using namespace testing;
using model::NewObject;
using model::ObjectId;

constexpr const char* owner = "fem.model.structural";

const nlohmann::json& samples() {
    static const nlohmann::json data = [] {
        std::ifstream stream(fs::path(FEM_TEST_DATA_DIR) / "structural" / "samples.json",
                             std::ios::binary);
        return nlohmann::json::parse(
            std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()));
    }();
    return data;
}

const nlohmann::json& sample(const std::string& type, std::size_t index) {
    return samples().at("valid").at(type).at(index);
}

NewObject object(const std::string& type, const std::string& name, const nlohmann::json& properties,
                 ObjectId parent = {}, std::vector<ObjectId> dependencies = {}) {
    NewObject init =
        model::testing::module_object(owner, type, name, parent, std::move(dependencies));
    init.properties = properties;
    return init;
}

ModuleVersions versions(const model::Document& document) {
    ModuleVersions result;
    for (const std::string& id : document.owners()) {
        result.emplace(id, id == model::core_owner ? "0.1.0" : "1.0.0");
    }
    return result;
}

/// A model in mm-t-s with an object of every type of a structural model for each sample of its
/// properties, in the places and with the dependencies of docs/extensions/fem.model.structural.md.
void build_model(TestDocument& doc) {
    doc.document.set_owner_enabled(owner, true);
    doc.edit.set_units(model::UnitSystem::mm_t_s());
    const ObjectId mesh =
        doc.edit.create(model::mesh_object("Part-1", model::testing::small_mesh()));
    const ObjectId fixed = doc.edit.create(model::node_set_object(
        mesh, "FIXED", std::make_shared<const model::NodeSet>(std::vector<model::Index>{0, 1})));
    const ObjectId solid = doc.edit.create(model::element_set_object(
        mesh, "SOLID", std::make_shared<const model::ElementSet>(std::vector<model::Index>{0, 1})));
    const ObjectId skin = doc.edit.create(model::element_set_object(
        mesh, "SKIN", std::make_shared<const model::ElementSet>(std::vector<model::Index>{2})));
    const ObjectId top = doc.edit.create(model::surface_object(
        mesh, "TOP",
        std::make_shared<const model::Surface>(std::vector<model::ElementSide>{{0, 1}, {1, 2}})));

    const ObjectId steel = doc.edit.create(object("material", "Сталь", sample("material", 0)));
    doc.edit.create(object("material", "AL-7075", sample("material", 1)));
    doc.edit.create(object("material", "soft", sample("material", 2)));

    doc.edit.create(
        object("solid_section", "body", sample("solid_section", 0), {}, {solid, steel}));
    doc.edit.create(object("solid_section", "", sample("solid_section", 1), {}, {solid, steel}));
    doc.edit.create(object("shell_section", "skin", sample("shell_section", 0), {}, {skin, steel}));
    doc.edit.create(object("shell_section", "", sample("shell_section", 1), {}, {skin, steel}));
    for (std::size_t i = 0; i < 3; ++i) {
        doc.edit.create(object("beam_section", "beam " + std::to_string(i),
                               sample("beam_section", i), {}, {skin, steel}));
    }

    // A boundary condition at the root acts in every step.
    doc.edit.create(
        object("boundary_condition", "clamp", sample("boundary_condition", 0), {}, {fixed}));
    const ObjectId step =
        doc.edit.create(object("static_step", "Step-1", sample("static_step", 0)));
    const ObjectId step2 =
        doc.edit.create(object("static_step", "Step-2", sample("static_step", 1)));
    doc.edit.create(
        object("boundary_condition", "settle", sample("boundary_condition", 1), step2, {fixed}));
    doc.edit.create(
        object("boundary_condition", "free", sample("boundary_condition", 2), step, {fixed}));
    doc.edit.create(
        object("concentrated_load", "tip", sample("concentrated_load", 0), step, {fixed}));
    doc.edit.create(
        object("concentrated_load", "", sample("concentrated_load", 1), step2, {fixed}));
    doc.edit.create(object("pressure", "on top", sample("pressure", 0), step, {top}));
    doc.edit.create(object("pressure", "on skin", sample("pressure", 1), step, {skin}));
    doc.edit.create(object("gravity", "weight", sample("gravity", 0), step, {solid}));
    doc.edit.create(object("gravity", "all", sample("gravity", 1), step2));
    doc.edit.create(object("output_request", "F-Output-1", sample("output_request", 0), step));
    doc.edit.create(object("output_request", "table", sample("output_request", 1), step, {fixed}));
}

std::shared_ptr<const model::ArrayData> arrays(std::vector<model::NamedArray> list) {
    return std::make_shared<const model::ArrayData>(std::move(list));
}

/// Values 0.5, 1.5... with a negative zero and a subnormal among them.
std::vector<double> values(std::size_t count) {
    std::vector<double> result(count);
    for (std::size_t i = 0; i < count; ++i) {
        result[i] = static_cast<double>(i) + 0.5;
    }
    result.front() = -0.0;
    result.back() = 5e-324;
    return result;
}

/// Results in their own document: the mesh they are on, a step, an increment and a field of
/// every position with its arrays.
void build_results(TestDocument& doc) {
    doc.document.set_owner_enabled(owner, true);
    doc.edit.set_units(model::UnitSystem::mm_t_s());
    const ObjectId mesh =
        doc.edit.create(model::mesh_object("Part-1", model::testing::small_mesh()));
    const ObjectId step =
        doc.edit.create(object("result_step", "Step-1", sample("result_step", 0)));
    const ObjectId frame =
        doc.edit.create(object("result_frame", "Increment 1", sample("result_frame", 0), step));
    doc.edit.create(object("result_frame", "Increment 0", sample("result_frame", 1), step));

    NewObject u = object("result_field", "U", sample("result_field", 0), frame, {mesh});
    {
        std::vector<model::NamedArray> list;
        list.emplace_back("node", std::vector<std::int32_t>{0, 1, 2, 3, 4});
        list.emplace_back("values", values(15), 3);
        u.data = arrays(std::move(list));
    }
    doc.edit.create(u);

    NewObject mises = object("result_field", "MISES", sample("result_field", 1), frame, {mesh});
    {
        std::vector<model::NamedArray> list;
        list.emplace_back("element", std::vector<std::int32_t>{0, 1, 2});
        list.emplace_back("values", values(3));
        mises.data = arrays(std::move(list));
    }
    doc.edit.create(mises);

    // One integration point in each tet4, three in the tri3.
    NewObject s = object("result_field", "S", sample("result_field", 2), frame, {mesh});
    {
        std::vector<model::NamedArray> list;
        list.emplace_back("element", std::vector<std::int32_t>{0, 1, 2, 2, 2});
        list.emplace_back("point", std::vector<std::int32_t>{1, 1, 1, 2, 3});
        list.emplace_back("values", values(30), 6);
        s.data = arrays(std::move(list));
    }
    doc.edit.create(s);

    // Every node of the tri3, as the element sees it.
    NewObject e = object("result_field", "E", sample("result_field", 3), frame, {mesh});
    {
        std::vector<model::NamedArray> list;
        list.emplace_back("element", std::vector<std::int32_t>{2, 2, 2});
        list.emplace_back("node", std::vector<std::int32_t>{0, 1, 2});
        list.emplace_back("values", values(18), 6);
        e.data = arrays(std::move(list));
    }
    doc.edit.create(e);
}

/// Saves the document, loads it and saves it again, in memory.
void expect_round_trip(const TestDocument& doc) {
    MemoryBlobStore blobs;
    const ModuleVersions modules = versions(doc.document);
    const ProjectFiles saved = write_project(doc.document, modules, blobs);
    const RoundTrip again(saved, blobs, modules);
    EXPECT_EQ(again.files, saved);
    EXPECT_EQ(describe(again.loaded.document), describe(doc.document));
}

TEST(StorageStructuralRoundTrip, EveryTypeOfTheModel) {
    TestDocument doc;
    build_model(doc);
    for (const std::string type :
         {"material", "solid_section", "shell_section", "beam_section", "static_step",
          "boundary_condition", "concentrated_load", "pressure", "gravity", "output_request"}) {
        EXPECT_EQ(doc.document.find_by_type(type).size(), samples().at("valid").at(type).size())
            << type;
    }
    expect_round_trip(doc);
}

TEST(StorageStructuralRoundTrip, ResultsWithFieldsOfEveryPosition) {
    TestDocument doc;
    build_results(doc);
    EXPECT_EQ(doc.document.find_by_type("result_field").size(), 4U);
    expect_round_trip(doc);
}

TEST(StorageStructuralRoundTrip, ObjectsOfTheDisabledModuleAreKeptAsTheyWere) {
    for (const auto build : {&build_model, &build_results}) {
        TestDocument doc;
        build(doc);
        doc.document.set_owner_enabled(owner, false);
        expect_round_trip(doc);
    }
}

TEST(StorageStructuralRoundTrip, ThroughAProjectFolder) {
    TempDir dir("structural");
    for (const auto build : {&build_model, &build_results}) {
        TestDocument doc;
        build(doc);
        const fs::path folder = dir.path() / fs::path(u8"модель балки.slproj");
        const ModuleVersions modules = versions(doc.document);
        (void)save_project(folder, doc.document, modules);
        const std::map<std::string, std::string> files = files_below(folder);

        TestDocument loaded;
        loaded.bus().replace_document(load_project(folder).contents);
        EXPECT_EQ(describe(loaded.document), describe(doc.document));
        const SaveStats stats = save_project(folder, loaded.document, modules);
        EXPECT_EQ(stats.written, 0U);
        EXPECT_EQ(stats.removed, 0U);
        EXPECT_EQ(files_below(folder), files);
        fs::remove_all(folder);
    }
}

TEST(StorageStructuralRoundTrip, AFieldFileSaysWhatItsArraysAre) {
    TestDocument doc;
    build_results(doc);
    MemoryBlobStore blobs;
    const ProjectFiles files = write_project(doc.document, versions(doc.document), blobs);
    const ObjectId u = doc.document.find_by_type("result_field").front();
    const std::string path =
        "model/ext/fem.model.structural/result_field/" + std::to_string(u.value()) + ".json";
    ASSERT_TRUE(files.contains(path));
    const nlohmann::json file = nlohmann::json::parse(files.at(path));
    EXPECT_EQ(file.at("data"),
              (nlohmann::json{{"kind", "arrays"},
                              {"size", 5},
                              {"arrays",
                               {{"node", {{"type", "int32"}, {"width", 1}}},
                                {"values", {{"type", "float64"}, {"width", 3}}}}}}));
    EXPECT_EQ(file.at("properties").at("quantity"), "length");
}

}  // namespace
}  // namespace fem::storage
