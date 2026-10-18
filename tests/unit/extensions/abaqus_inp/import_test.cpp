// An input file as objects of the Model IR (task 3.2, docs/extensions/fem.format.abaqus-inp.md):
// the fixtures of tests/data/inp, imported without the platform. That the objects reach a
// document through the C ABI: module_test.cpp.

#include "inp/import.h"

#include "fem/core/model/element_type.h"

#include "inp/elements.h"
#include "inp_test_support.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace fem::inp {
namespace {

using nlohmann::json;
using testing::find;
using testing::has_problem;
using testing::import_fixture;
using testing::indices;
using testing::keyword_item;
using testing::of_type;
using testing::refusal;
using testing::utf8;

const MeshData& mesh(const Import& result, ItemRef item) {
    return std::get<MeshData>(result.items.at(item).data);
}

std::vector<ItemRef> deps(const Import& result, ItemRef item) {
    return result.items.at(item).dependencies;
}

TEST(InpElements, FacesMatchTheSidesOfTheModel) {
    // Each Abaqus face of a solid is the side of the neutral shape with the same corners.
    for (const auto& [shape, type, faces] : {std::tuple{"tet4", model::ElementType::tet4, 4},
                                             std::tuple{"hex8", model::ElementType::hex8, 6},
                                             std::tuple{"wedge6", model::ElementType::wedge6, 5}}) {
        const auto& info = model::element_type_info(type);
        for (int face = 1; face <= faces; ++face) {
            SCOPED_TRACE(std::string(shape) + " S" + std::to_string(face));
            const auto corners = abaqus_face_corners(shape, face);
            const std::optional<std::int32_t> side = neutral_side(shape, face);
            ASSERT_TRUE(side.has_value());
            const auto neutral = info.sides[static_cast<std::size_t>(*side)].corners();
            EXPECT_EQ(std::set<int>(corners.begin(), corners.end()),
                      std::set<int>(neutral.begin(), neutral.end()));
        }
        EXPECT_FALSE(neutral_side(shape, faces + 1).has_value());
    }
    // Every type the importer reads has a shape of the Model IR with its number of nodes.
    for (const char* type : {"C3D4", "C3D10M", "C3D8R", "C3D20R", "C3D6", "C3D15", "S3", "S4R",
                             "S6", "S8R", "B31", "B32"}) {
        const std::optional<ElementKind> kind = element_kind(type);
        ASSERT_TRUE(kind.has_value()) << type;
        const auto shape = model::parse_element_type(kind->shape);
        ASSERT_TRUE(shape.has_value()) << type;
        EXPECT_EQ(model::nodes_per_element(*shape), kind->nodes) << type;
    }
    EXPECT_FALSE(element_kind("CPS4").has_value());
}

TEST(InpImport, FlatModelGivesEveryKindOfObject) {
    const Import r = import_fixture("flat.inp");
    ASSERT_FALSE(r.items.empty());
    const Item& input = r.items[r.input];
    EXPECT_EQ(input.type, "abaqus_input");
    EXPECT_EQ(input.owner, "fem.format.abaqus-inp");
    EXPECT_EQ(input.properties,
              (json{{"file", "flat.inp"},
                    {"heading", "Two cubes, clamped at x = 0\nSecond line of the heading"},
                    {"steps", {"Load"}}}));

    // The material, with what the model does not hold as its child.
    const ItemRef steel = find(r, "material", "Steel");
    ASSERT_NE(steel, no_item);
    EXPECT_EQ(r.items[steel].owner, "fem.model.structural");
    EXPECT_EQ(r.items[steel].properties,
              (json{{"youngsModulus", 210000.0}, {"poissonsRatio", 0.3}, {"density", 7.85e-9}}));
    const ItemRef plastic = keyword_item(r, "*PLASTIC");
    ASSERT_NE(plastic, no_item);
    EXPECT_EQ(r.items[plastic].parent, steel);
    EXPECT_EQ(r.items[plastic].properties, (json{{"keyword", "*PLASTIC"},
                                                 {"text", "*PLASTIC\n250., 0."},
                                                 {"file", "flat.inp"},
                                                 {"line", 35},
                                                 {"index", 10},
                                                 {"material", "Steel"}}));

    // The mesh, named after the file, with the element type of each block.
    const ItemRef flat = find(r, "mesh", "flat");
    ASSERT_NE(flat, no_item);
    const MeshData& m = mesh(r, flat);
    EXPECT_EQ(m.node_ids.size(), 12U);
    EXPECT_EQ(m.x[2], 2.0);
    ASSERT_EQ(m.blocks.size(), 1U);
    EXPECT_EQ(m.blocks[0].shape, "hex8");
    EXPECT_EQ(m.blocks[0].ids, (std::vector<std::int64_t>{1, 2}));
    EXPECT_EQ(std::vector<std::int32_t>(m.blocks[0].connectivity.begin(),
                                        m.blocks[0].connectivity.begin() + 8),
              (std::vector<std::int32_t>{0, 1, 4, 3, 6, 7, 10, 9}));
    const ItemRef types = of_type(r, "abaqus_element_types", flat).at(0);
    EXPECT_EQ(r.items[types].properties, (json{{"blocks", {"C3D8"}}}));

    // Sets by index, GENERATE expanded, the face S2 of element 2 as side 5 of hex8.
    EXPECT_EQ(indices(r, find(r, "node_set", "NALL", flat)).size(), 12U);
    EXPECT_EQ(indices(r, find(r, "element_set", "EALL", flat)), (std::vector<std::int32_t>{0, 1}));
    EXPECT_EQ(indices(r, find(r, "node_set", "FIX", flat)),
              (std::vector<std::int32_t>{0, 3, 6, 9}));
    EXPECT_EQ(indices(r, find(r, "node_set", "TIP", flat)),
              (std::vector<std::int32_t>{2, 5, 8, 11}));
    EXPECT_EQ(indices(r, find(r, "surface", "TOP", flat)), (std::vector<std::int32_t>{1, 5}));

    const ItemRef section = find(r, "solid_section", "EALL");
    ASSERT_NE(section, no_item);
    EXPECT_EQ(r.items[section].properties, (json{{"integration", "full"}}));
    EXPECT_EQ(deps(r, section), (std::vector<ItemRef>{find(r, "element_set", "EALL"), steel}));

    // A boundary condition of the model data acts in every step: at the root.
    const ItemRef fix = find(r, "boundary_condition", "FIX");
    ASSERT_NE(fix, no_item);
    EXPECT_EQ(r.items[fix].parent, no_item);
    EXPECT_EQ(r.items[fix].properties, (json{{"ux", 0.0}, {"uy", 0.0}, {"uz", 0.0}}));
    EXPECT_EQ(deps(r, fix), (std::vector<ItemRef>{find(r, "node_set", "FIX")}));

    const ItemRef step = find(r, "static_step", "Load");
    ASSERT_NE(step, no_item);
    EXPECT_EQ(r.items[step].properties,
              (json{{"maxIncrements", 50}, {"timePeriod", 1.0}, {"initialIncrement", 0.1}}));

    // Loads: on a set; on a node by label, its lines merged into one set the importer makes.
    const ItemRef tip = find(r, "concentrated_load", "TIP", step);
    ASSERT_NE(tip, no_item);
    EXPECT_EQ(r.items[tip].properties, (json{{"fz", -10.0}}));
    const ItemRef node9 = find(r, "concentrated_load", "_Cload-1", step);
    ASSERT_NE(node9, no_item);
    EXPECT_EQ(r.items[node9].properties, (json{{"fx", 2.5}, {"fy", 1.0}}));
    EXPECT_EQ(indices(r, deps(r, node9).at(0)), (std::vector<std::int32_t>{8}));

    const ItemRef gravity = find(r, "gravity", "EALL", step);
    ASSERT_NE(gravity, no_item);
    EXPECT_EQ(
        r.items[gravity].properties,
        (json{{"acceleration", 9810.0}, {"direction", {{"x", 0.0}, {"y", 0.0}, {"z", -1.0}}}}));
    // P1 on element 1 is a pressure on a surface of its face S1, side 4 of hex8.
    const ItemRef face = find(r, "pressure", "_Dload-1", step);
    ASSERT_NE(face, no_item);
    EXPECT_EQ(r.items[face].properties, (json{{"magnitude", 0.5}}));
    EXPECT_EQ(r.items[deps(r, face).at(0)].type, "surface");
    EXPECT_EQ(indices(r, deps(r, face).at(0)), (std::vector<std::int32_t>{0, 4}));
    const ItemRef top = find(r, "pressure", "TOP", step);
    ASSERT_NE(top, no_item);
    EXPECT_EQ(deps(r, top), (std::vector<ItemRef>{find(r, "surface", "TOP")}));

    // Output requests; the variable the model does not have is kept.
    const ItemRef nodal = find(r, "output_request", "Node output", step);
    ASSERT_NE(nodal, no_item);
    EXPECT_EQ(r.items[nodal].properties, (json{{"displacement", true},
                                               {"reactionForce", true},
                                               {"stress", false},
                                               {"strain", false},
                                               {"destination", "results"},
                                               {"frequency", 1}}));
    const ItemRef element = find(r, "output_request", "Element output", step);
    ASSERT_NE(element, no_item);
    EXPECT_TRUE(r.items[element].properties.at("stress").get<bool>());
    EXPECT_TRUE(r.items[element].properties.at("strain").get<bool>());
    const ItemRef ener = keyword_item(r, "*EL FILE");
    ASSERT_NE(ener, no_item);
    EXPECT_EQ(r.items[ener].properties.at("text"), "*EL FILE\nENER");
    EXPECT_EQ(r.items[ener].parent, step);

    // Passthrough in the step and in the model data, with their places.
    const ItemRef contact = keyword_item(r, "*CONTACT CONTROLS");
    ASSERT_NE(contact, no_item);
    EXPECT_EQ(r.items[contact].parent, step);
    EXPECT_EQ(r.items[contact].name, "*CONTACT CONTROLS (flat.inp:54)");
    EXPECT_EQ(r.items[contact].properties.at("text"), "*CONTACT CONTROLS, ADD");
    const ItemRef amplitude = keyword_item(r, "*AMPLITUDE");
    ASSERT_NE(amplitude, no_item);
    EXPECT_EQ(r.items[amplitude].parent, r.input);
    EXPECT_EQ(r.items[amplitude].properties.at("line"), 38);

    EXPECT_TRUE(has_problem(r, "problem.passthrough", "keyword", "*CONTACT CONTROLS"));
    EXPECT_TRUE(has_problem(r, "problem.passthrough", "keyword", "*PLASTIC"));
    EXPECT_TRUE(has_problem(r, "problem.passthrough", "keyword", "*AMPLITUDE"));
    EXPECT_TRUE(has_problem(r, "problem.unsupportedVariables", "variables", "ENER"));
    EXPECT_EQ(r.problems.back().key, "problem.units");
    EXPECT_EQ(r.problems.back().severity, Problem::Severity::info);
}

TEST(InpImport, ItemsComeAfterTheirParentsAndDependencies) {
    for (const char* file : {"flat.inp", "assembly.inp", "mixed.inp", "unsupported.inp"}) {
        SCOPED_TRACE(file);
        const Import r = import_fixture(file);
        for (ItemRef i = 0; i < r.items.size(); ++i) {
            const Item& item = r.items[i];
            EXPECT_TRUE(item.parent == no_item || item.parent < i) << item.type << " " << item.name;
            for (const ItemRef target : item.dependencies) {
                EXPECT_LT(target, i) << item.type << " " << item.name;
            }
        }
    }
}

TEST(InpImport, PlacesInTheFileAreKeptForTheExport) {
    const Import result = import_fixture("assembly.inp");
    const Item& input = result.items[result.input];
    EXPECT_EQ(input.properties.at("assembly"), "Assembly");
    EXPECT_EQ(input.properties.at("steps"), json::array({"Step-1"}));

    const auto types_of = [&](std::string_view mesh) -> const json& {
        return result.items[find(result, "abaqus_element_types", "", find(result, "mesh", mesh))]
            .properties;
    };
    // An instance knows its part and which of its sets are the part's; the rest are the assembly's.
    const json& beam = types_of("Beam-1");
    EXPECT_EQ(beam.at("part"), "Beam");
    EXPECT_EQ(beam.at("instance"), "Beam-1");
    EXPECT_EQ(beam.at("partSets"), json::array({"Set-1", "Set-1"}));  // Nodes and elements.
    EXPECT_FALSE(beam.contains("assembly"));
    EXPECT_EQ(types_of("Assembly").at("assembly"), true);
    EXPECT_FALSE(types_of("Assembly").contains("part"));

    // A keyword kept in a step knows how many objects of the step come before it.
    const Item& restart = result.items[keyword_item(result, "*RESTART")];
    EXPECT_EQ(restart.properties.at("position"), 3);  // The load, the pressure, the gravity.
    EXPECT_FALSE(result.items[keyword_item(result, "*PREPRINT")].properties.contains("position"));
}

TEST(InpImport, InstancesAreMeshesInTheirPlaces) {
    const Import r = import_fixture("assembly.inp");
    const ItemRef first = find(r, "mesh", "Beam-1");
    const ItemRef second = find(r, "mesh", "Beam-2");
    const ItemRef assembly = find(r, "mesh", "Assembly");
    ASSERT_NE(first, no_item);
    ASSERT_NE(second, no_item);
    ASSERT_NE(assembly, no_item);
    EXPECT_EQ(of_type(r, "mesh").size(), 3U);

    // Beam-1 where the part is; Beam-2 moved by 10 along x, then turned by 90° about the z axis
    // through (10, 0, 0).
    EXPECT_EQ(mesh(r, first).x, (std::vector<double>{0.0, 1.0, 0.0, 0.0}));
    const MeshData& moved = mesh(r, second);
    const std::vector<std::array<double, 3>> expected = {
        {10.0, 0.0, 0.0}, {10.0, 1.0, 0.0}, {9.0, 0.0, 0.0}, {10.0, 0.0, 1.0}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(moved.x[i], expected[i][0], 1e-12) << i;
        EXPECT_NEAR(moved.y[i], expected[i][1], 1e-12) << i;
        EXPECT_NEAR(moved.z[i], expected[i][2], 1e-12) << i;
    }
    EXPECT_EQ(moved.node_ids, (std::vector<std::int64_t>{1, 2, 3, 4}));
    EXPECT_EQ(mesh(r, assembly).x, (std::vector<double>{5.0}));

    // Each instance has the sets of its part; an assembly set named like one of them is renamed.
    EXPECT_EQ(indices(r, find(r, "node_set", "Set-1", first)), (std::vector<std::int32_t>{0, 1}));
    EXPECT_EQ(indices(r, find(r, "element_set", "Set-1", second)), (std::vector<std::int32_t>{0}));
    EXPECT_EQ(indices(r, find(r, "node_set", "Assembly.Set-1", first)),
              (std::vector<std::int32_t>{0}));
    EXPECT_EQ(find(r, "node_set", "Assembly.Set-1", second), no_item);
    EXPECT_TRUE(has_problem(r, "problem.renamedSet", "name", "Assembly.Set-1"));
    EXPECT_EQ(indices(r, find(r, "node_set", "RP", assembly)), (std::vector<std::int32_t>{0}));
    // A surface of an assembly element set on Beam-2: face S1 of the tet is side 3.
    EXPECT_EQ(indices(r, find(r, "surface", "Surf-1", second)), (std::vector<std::int32_t>{0, 3}));

    // The section of the part: one per instance, on the instance's set; the material comes
    // after the part in the file.
    const ItemRef steel = find(r, "material", "Steel");
    for (const auto& [name, instance] :
         {std::pair{"Beam-1.Set-1", first}, std::pair{"Beam-2.Set-1", second}}) {
        const ItemRef section = find(r, "solid_section", name);
        ASSERT_NE(section, no_item) << name;
        EXPECT_EQ(deps(r, section),
                  (std::vector<ItemRef>{find(r, "element_set", "Set-1", instance), steel}));
    }

    // A boundary condition on a set of the assembly that is on both instances.
    const ItemRef fixed = find(r, "boundary_condition", "Fixed");
    ASSERT_NE(fixed, no_item);
    EXPECT_EQ(r.items[fixed].properties.size(), 6U);
    EXPECT_EQ(deps(r, fixed), (std::vector<ItemRef>{find(r, "node_set", "Fixed", first),
                                                    find(r, "node_set", "Fixed", second)}));

    const ItemRef step = find(r, "static_step", "Step-1");
    ASSERT_NE(step, no_item);
    // "Beam-2.4": node 4 of Beam-2.
    const ItemRef load = find(r, "concentrated_load", "_Cload-1", step);
    ASSERT_NE(load, no_item);
    EXPECT_EQ(r.items[deps(r, load).at(0)].parent, second);
    EXPECT_EQ(indices(r, deps(r, load).at(0)), (std::vector<std::int32_t>{3}));
    const ItemRef pressure = find(r, "pressure", "Surf-1", step);
    ASSERT_NE(pressure, no_item);
    EXPECT_EQ(deps(r, pressure), (std::vector<ItemRef>{find(r, "surface", "Surf-1", second)}));
    const ItemRef gravity = find(r, "gravity", "Gravity", step);
    ASSERT_NE(gravity, no_item);
    EXPECT_TRUE(deps(r, gravity).empty());
    const ItemRef output = find(r, "output_request", "Field output", step);
    ASSERT_NE(output, no_item);
    EXPECT_TRUE(r.items[output].properties.at("strain").get<bool>());

    // History output is kept with the keywords that belong to it.
    const ItemRef history = keyword_item(r, "*OUTPUT");
    ASSERT_NE(history, no_item);
    EXPECT_EQ(r.items[history].properties.at("text"),
              "*Output, history, variable=PRESELECT\n*Node Output, nset=Set-1\nU");
    EXPECT_TRUE(has_problem(r, "problem.passthrough", "keyword", "*RESTART"));
    EXPECT_TRUE(has_problem(r, "problem.passthrough", "keyword", "*PREPRINT"));
    EXPECT_FALSE(has_problem(r, "problem.nlgeom", "step", "Step-1"));
}

TEST(InpImport, IncludedFilesAreReadInPlace) {
    const Import r = import_fixture(std::filesystem::path("include") / "main.inp");
    const ItemRef m = find(r, "mesh", "main");
    ASSERT_NE(m, no_item);
    EXPECT_EQ(mesh(r, m).node_ids.size(), 4U);
    EXPECT_EQ(indices(r, find(r, "node_set", "ALLN", m)), (std::vector<std::int32_t>{0, 1, 2, 3}));
    EXPECT_EQ(indices(r, find(r, "element_set", "ALL", m)), (std::vector<std::int32_t>{0}));
    const ItemRef step = find(r, "static_step", "Step-1");
    ASSERT_NE(step, no_item);
    EXPECT_NE(find(r, "boundary_condition", "_Boundary-1", step), no_item);
    const ItemRef load = find(r, "concentrated_load", "_Cload-1", step);
    ASSERT_NE(load, no_item);
    EXPECT_EQ(r.items[load].properties, (json{{"fz", -1.0}}));

    const ItemRef contact = keyword_item(r, "*CONTACT CONTROLS");
    ASSERT_NE(contact, no_item);
    EXPECT_EQ(r.items[contact].properties.at("file"), utf8(u8"папка с пробелами/шаг.inp"));
    EXPECT_EQ(r.items[contact].properties.at("line"), 5);
    EXPECT_EQ(r.items[contact].properties.at("index"), 9);
}

TEST(InpImport, EncodingsGiveTheSameHeading) {
    const std::string heading = utf8(u8"Кронштейн №1 — «тест»");
    for (const char* file : {"utf8_bom.inp", "utf16le.inp", "utf16be.inp"}) {
        const Import r = import_fixture(std::filesystem::path("bytes") / file);
        EXPECT_EQ(r.items.at(r.input).properties.at("heading"), heading) << file;
        EXPECT_FALSE(
            testing::has_problem(r, "problem.encodingFallback", "encoding", "windows-1252"));
        EXPECT_EQ(
            mesh(r, find(r, "mesh", std::filesystem::path(file).stem().string())).node_ids.size(),
            4U);
    }
    const Import named = import_fixture("bytes/cp1251.inp", Encoding::windows1251);
    EXPECT_EQ(named.items.at(named.input).properties.at("heading"), heading);

    // Without a name the bytes are read as Windows-1252, with a warning.
    const Import guessed = import_fixture("bytes/cp1251.inp");
    const std::string text = guessed.items.at(guessed.input).properties.at("heading");
    EXPECT_NE(text, heading);
    EXPECT_TRUE(is_valid_utf8(text));
    ASSERT_TRUE(has_problem(guessed, "problem.encodingFallback", "encoding", "windows-1252"));
    EXPECT_EQ(guessed.problems.front().file, "cp1251.inp");
}

TEST(InpImport, IntegrationAndKindOfSectionFollowTheElementTypes) {
    const Import r = import_fixture("mixed.inp");
    const ItemRef m = find(r, "mesh", "mixed");
    ASSERT_NE(m, no_item);
    std::vector<std::string> shapes;
    for (const MeshBlock& block : mesh(r, m).blocks) {
        shapes.push_back(block.shape);
    }
    EXPECT_EQ(shapes, (std::vector<std::string>{"hex8", "hex8", "quad4", "line2", "hex8"}));
    EXPECT_EQ(r.items[of_type(r, "abaqus_element_types", m).at(0)].properties.at("blocks"),
              (json{"C3D8", "C3D8R", "S4R", "B31", "C3D8I"}));

    // C3D8 and C3D8R in one set: the section is split by integration.
    EXPECT_EQ(indices(r, find(r, "element_set", "SOLIDS-full", m)), (std::vector<std::int32_t>{0}));
    EXPECT_EQ(indices(r, find(r, "element_set", "SOLIDS-reduced", m)),
              (std::vector<std::int32_t>{1}));
    const ItemRef full = find(r, "solid_section", "SOLIDS-full");
    const ItemRef reduced = find(r, "solid_section", "SOLIDS-reduced");
    ASSERT_NE(full, no_item);
    ASSERT_NE(reduced, no_item);
    EXPECT_EQ(r.items[full].properties.at("integration"), "full");
    EXPECT_EQ(r.items[reduced].properties.at("integration"), "reduced");
    EXPECT_EQ(deps(r, reduced).at(0), find(r, "element_set", "SOLIDS-reduced", m));
    EXPECT_TRUE(has_problem(r, "problem.mixedIntegration", "set", "SOLIDS"));

    const ItemRef shell = find(r, "shell_section", "SKIN");
    ASSERT_NE(shell, no_item);
    EXPECT_EQ(r.items[shell].properties, (json{{"thickness", 2.0},
                                               {"integrationPoints", 7},
                                               {"offset", 0.5},
                                               {"integration", "reduced"}}));
    const ItemRef beam = find(r, "beam_section", "BEAM");
    ASSERT_NE(beam, no_item);
    EXPECT_EQ(r.items[beam].properties,
              (json{{"shape", "rectangle"},
                    {"width", 0.5},
                    {"height", 1.0},
                    {"direction", {{"x", 0.0}, {"y", 1.0}, {"z", 0.0}}}}));

    // A solid section on shells, and a type read as its shape only, are warned about.
    EXPECT_TRUE(has_problem(r, "problem.sectionFamily", "type", "S4R"));
    EXPECT_TRUE(has_problem(r, "problem.elementType", "type", "C3D8I"));
    EXPECT_FALSE(has_problem(r, "problem.elementType", "type", "C3D8R"));
}

TEST(InpImport, WhatTheModelCannotHoldIsKeptWithAProblem) {
    const Import r = import_fixture("unsupported.inp");
    EXPECT_TRUE(has_problem(r, "problem.unsupportedElementType", "type", "CPS4"));
    EXPECT_TRUE(has_problem(r, "problem.unsupportedSurface", "name", "NODES"));
    EXPECT_TRUE(has_problem(r, "problem.temperatureDependent", "material", "Hot"));
    EXPECT_TRUE(has_problem(r, "problem.unknownReference", "name", "NOWHERE"));
    EXPECT_TRUE(has_problem(r, "problem.unknownReference", "name", "Missing"));
    EXPECT_TRUE(has_problem(r, "problem.unsupportedStep", "procedure", "*FREQUENCY"));
    EXPECT_TRUE(has_problem(r, "problem.nlgeom", "step", "Static"));
    EXPECT_TRUE(has_problem(r, "problem.unsupportedParameter", "parameter", "OP=NEW"));
    EXPECT_TRUE(has_problem(r, "problem.unsupportedParameter", "parameter", "AMPLITUDE"));
    EXPECT_TRUE(has_problem(r, "problem.unsupportedLines", "keyword", "*BOUNDARY"));
    EXPECT_TRUE(has_problem(r, "problem.dependsOnPassthrough", "name", "NODES"));

    const auto texts = [&](ItemRef parent) {
        std::vector<std::string> found;
        for (const ItemRef i : of_type(r, "abaqus_keyword", parent)) {
            found.push_back(r.items[i].properties.at("text"));
        }
        return found;
    };
    EXPECT_EQ(texts(r.input), (std::vector<std::string>{
                                  "*ELEMENT, TYPE=CPS4, ELSET=PLANE\n2, 1, 2, 3, 4",
                                  "*SURFACE, NAME=NODES, TYPE=NODE\nALL, 1.",
                                  "*SOLID SECTION, ELSET=NOWHERE, MATERIAL=Hot",
                                  "*SOLID SECTION, ELSET=SOLID, MATERIAL=Missing",
                                  "*STEP, NAME=Modes\n*FREQUENCY\n10\n*END STEP",
                              }));
    const ItemRef hot = find(r, "material", "Hot");
    EXPECT_EQ(texts(hot),
              (std::vector<std::string>{"*ELASTIC\n200000., 0.3, 20.\n190000., 0.3, 200."}));
    EXPECT_TRUE(r.items[hot].properties.empty());
    const ItemRef step = find(r, "static_step", "Static");
    ASSERT_NE(step, no_item);
    EXPECT_EQ(texts(step), (std::vector<std::string>{
                               "*BOUNDARY, OP=NEW\nALL, 1, 3", "*BOUNDARY\nALL, 11, 11, 20.",
                               "*CLOAD, AMPLITUDE=RAMP\n4, 3, 1.", "*DSLOAD\nNODES, P, 1."}));
    // The supported line of the second *BOUNDARY stays a boundary condition.
    const ItemRef bc = find(r, "boundary_condition", "_Boundary-1", step);
    ASSERT_NE(bc, no_item);
    EXPECT_EQ(indices(r, deps(r, bc).at(0)), (std::vector<std::int32_t>{0}));
    EXPECT_EQ(of_type(r, "concentrated_load").size(), 0U);
    EXPECT_EQ(of_type(r, "pressure").size(), 0U);
    EXPECT_NE(find(r, "solid_section", "SOLID"), no_item);
}

TEST(InpImport, ProblemsOfOneKeywordAreCounted) {
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "stresslab-inp-repeated.inp";
    {
        std::ofstream out(file, std::ios::binary);
        out << "*NODE\n1, 0, 0, 0\n*PREPRINT\n*PREPRINT\n*PREPRINT\n";
    }
    const Import r = import_file(file, Encoding::automatic);
    std::filesystem::remove(file);
    const auto it = std::find_if(r.problems.begin(), r.problems.end(),
                                 [](const Problem& p) { return p.args.contains("keyword"); });
    ASSERT_NE(it, r.problems.end());
    EXPECT_EQ(it->key, "problem.passthroughRepeated");
    EXPECT_EQ(it->args.at("count"), 3);
    EXPECT_EQ(it->line, 3);
    EXPECT_EQ(of_type(r, "abaqus_keyword").size(), 3U);
}

TEST(InpImport, RefusesFilesWithBrokenMeshes) {
    struct Case {
        const char* file;
        const char* key;
        std::int64_t line;
        json args;
    };
    const std::vector<Case> cases = {
        {"unknown_node.inp", "error.unknownNode", 8, {{"node", 99}, {"element", 2}}},
        {"duplicate_node.inp", "error.duplicateNode", 6, {{"node", 4}}},
        {"bad_number.inp", "error.badNumber", 2, {{"value", "abc"}, {"keyword", "*NODE"}}},
        {"incomplete_element.inp",
         "error.incompleteElement",
         7,
         {{"element", 1}, {"type", "C3D20"}, {"count", 20}}},
        {"unclosed_part.inp", "error.unclosed", 2, {{"keyword", "*PART"}}},
        {"unknown_part.inp", "error.unknownPart", 2, {{"part", "Nothing"}}},
        {"parameter.inp", "error.unsupportedKeyword", 1, {{"keyword", "*PARAMETER"}}},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.file);
        const Problem problem = refusal(std::filesystem::path("errors") / c.file);
        EXPECT_EQ(problem.key, c.key);
        EXPECT_EQ(problem.severity, Problem::Severity::error);
        EXPECT_EQ(problem.file, c.file);
        EXPECT_EQ(problem.line, c.line);
        EXPECT_EQ(problem.args, c.args);
    }
}

TEST(InpImport, ProblemOfAnIncludedDataLineNamesTheIncludedFile) {
    // *NODE is in the main file, its data lines in the included one.
    const Problem problem = refusal("errors/included_bad_number.inp");
    EXPECT_EQ(problem.key, "error.badNumber");
    EXPECT_EQ(problem.file, "bad_nodes.inp");
    EXPECT_EQ(problem.line, 2);
    EXPECT_EQ(problem.args.at("value"), "x1");
}

}  // namespace
}  // namespace fem::inp
