// The export of fem.format.abaqus-inp (task 3.3) without the platform: a fixture imported, written
// and imported again gives the same model by meaning, with the kept keywords in their places; a
// second export gives the same bytes; describe_losses names what the file cannot hold.

#include "inp/export.h"

#include "inp/model.h"
#include "inp/numbers.h"
#include "inp_test_support.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::inp::Export;
using fem::inp::Import;
using fem::inp::Item;
using fem::inp::MeshView;
using fem::inp::Model;
using fem::inp::ModelObject;
using fem::inp::Problem;
using fem::inp::SetKind;
using fem::inp::SetView;
using fem::inp::testing::import_fixture;
using Json = nlohmann::json;

// --- The meaning of a model --------------------------------------------------------------------

/// What a model means, as the export must keep it: meshes by name with nodes and elements by
/// label, sets by their labels, objects of fem.model.structural with their targets, the kept
/// keywords in the order of each place. Ids, file names, lines and the names the file does not
/// give (sections, loads and output requests are named by the import after their targets) are
/// left out.
class Meaning {
public:
    explicit Meaning(const Model& model) : model_(model) {
        for (const ModelObject& object : model.objects) {
            by_id_.emplace(object.id, &object);
        }
    }

    Json operator()() const {
        Json out = Json::object();
        Json meshes = Json::object();
        Json materials = Json::object();
        Json sections = Json::array();
        Json model_loads = Json::array();
        Json steps = Json::object();
        Json kept = Json::object();
        Json order = Json::array();
        for (const ModelObject& object : model_.objects) {
            const std::string& type = object.type;
            if (type == "mesh") {
                meshes[object.name] = mesh(object);
            } else if (type == "material") {
                materials[object.name] = object.properties;
            } else if (type.ends_with("_section")) {
                sections.push_back({{"type", type},
                                    {"properties", object.properties},
                                    {"targets", targets(object)}});
            } else if (type == "static_step") {
                steps[object.name] = step(object);
            } else if (type == "abaqus_input") {
                out["heading"] = object.properties.value("heading", "");
                out["assembly"] = object.properties.value("assembly", "");
                for (const Json& name : object.properties.value("steps", Json::array())) {
                    order.push_back(name);
                }
            } else if (type == "abaqus_keyword") {
                const ModelObject& parent = *by_id_.at(object.parent);
                if (parent.type != "static_step") {
                    Json place = object.properties;
                    for (const char* key : {"text", "file", "line", "index", "keyword"}) {
                        place.erase(key);
                    }
                    place["parent"] =
                        parent.type == "material" ? "material " + parent.name : parent.type;
                    kept[place.dump()].push_back(object.properties.at("text"));
                }
            } else if (object.parent == 0 && object.owner == "fem.model.structural") {
                model_loads.push_back(load(object));
            }
        }
        std::sort(sections.begin(), sections.end());
        std::sort(model_loads.begin(), model_loads.end());
        out["meshes"] = meshes;
        out["materials"] = materials;
        out["sections"] = sections;
        out["boundary"] = model_loads;
        out["steps"] = steps;
        out["stepOrder"] = order;
        out["kept"] = kept;
        return out;
    }

private:
    [[nodiscard]] const ModelObject* child(const ModelObject& parent, std::string_view type) const {
        for (const ModelObject& object : model_.objects) {
            if (object.parent == parent.id && object.type == type) {
                return &object;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Json mesh(const ModelObject& object) const {
        const MeshView& view = std::get<MeshView>(object.data);
        Json nodes = Json::object();
        for (std::size_t i = 0; i < view.node_ids.size(); ++i) {
            nodes[std::to_string(view.node_ids[i])] = {view.x[i], view.y[i], view.z[i]};
        }
        const ModelObject* types = child(object, "abaqus_element_types");
        Json place = types != nullptr ? types->properties : Json::object();
        Json elements = Json::object();
        for (std::size_t b = 0; b < view.blocks.size(); ++b) {
            const auto& block = view.blocks[b];
            const auto n = block.ids.empty() ? 0 : block.connectivity.size() / block.ids.size();
            const Json type = types != nullptr ? place.at("blocks").at(b) : Json(block.shape);
            for (std::size_t e = 0; e < block.ids.size(); ++e) {
                Json connected = Json::array();
                for (std::size_t k = 0; k < n; ++k) {
                    connected.push_back(view.node_ids[block.connectivity[e * n + k]]);
                }
                elements[std::to_string(block.ids[e])] = {type, connected};
            }
        }
        place.erase("blocks");
        if (place.contains("partSets")) {
            std::sort(place["partSets"].begin(), place["partSets"].end());
        }
        Json sets = Json::object();
        for (const ModelObject& set : model_.objects) {
            if (set.parent == object.id && std::holds_alternative<SetView>(set.data)) {
                sets[set.type + " " + set.name] = members(set, view);
            }
        }
        return {{"nodes", nodes}, {"elements", elements}, {"place", place}, {"sets", sets}};
    }

    static Json members(const ModelObject& set, const MeshView& view) {
        const SetView& data = std::get<SetView>(set.data);
        std::vector<std::int64_t> element_labels;
        for (const auto& block : view.blocks) {
            element_labels.insert(element_labels.end(), block.ids.begin(), block.ids.end());
        }
        Json out = Json::array();
        if (data.kind == SetKind::surface) {
            for (std::size_t p = 0; p + 1 < data.indices.size(); p += 2) {
                out.push_back({element_labels[data.indices[p]], data.indices[p + 1]});
            }
        } else {
            for (const std::int32_t index : data.indices) {
                out.push_back(data.kind == SetKind::node ? view.node_ids[index]
                                                         : element_labels[index]);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    /// The targets of an object as "mesh/type name", sorted.
    [[nodiscard]] Json targets(const ModelObject& object) const {
        Json out = Json::array();
        for (const std::int64_t id : object.dependencies) {
            const ModelObject& target = *by_id_.at(id);
            const std::string owner_name =
                target.parent != 0 ? by_id_.at(target.parent)->name + "/" : std::string();
            out.push_back(owner_name + target.type + " " + target.name);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    [[nodiscard]] Json load(const ModelObject& object) const {
        return {
            {"type", object.type}, {"properties", object.properties}, {"targets", targets(object)}};
    }

    [[nodiscard]] Json step(const ModelObject& object) const {
        Json children = Json::array();
        Json kept = Json::array();
        for (const ModelObject& child : model_.objects) {
            if (child.parent != object.id) {
                continue;
            }
            if (child.type == "abaqus_keyword") {
                kept.push_back(
                    {child.properties.value("position", 0), child.properties.at("text")});
            } else {
                children.push_back(load(child));
            }
        }
        return {{"properties", object.properties}, {"objects", children}, {"kept", kept}};
    }

    const Model& model_;
    std::map<std::int64_t, const ModelObject*> by_id_;
};

Json meaning(const Import& result) {
    const Model model = fem::inp::model_of(result);
    return Meaning(model)();
}

/// A folder of its own for the files of a test, with Cyrillic letters and spaces in its path.
fs::path scratch() {
    std::random_device random;
    const fs::path folder = fs::temp_directory_path() / "stresslab-inp-export" /
                            std::to_string(random()) / fs::path(u8"экспорт inp");
    fs::create_directories(folder);
    return folder;
}

void write(const fs::path& file, const std::string& text) {
    std::ofstream stream(file, std::ios::binary);
    stream << text;
}

bool has_loss(const std::vector<Problem>& losses, std::string_view key) {
    return std::any_of(losses.begin(), losses.end(),
                       [&](const Problem& p) { return p.key == key; });
}

std::vector<std::string> loss_keys(const std::vector<Problem>& losses) {
    std::vector<std::string> keys;
    for (const Problem& problem : losses) {
        keys.push_back(problem.key);
    }
    return keys;
}

// --- Round trip --------------------------------------------------------------------------------

class InpRoundTrip : public ::testing::TestWithParam<const char*> {};

TEST_P(InpRoundTrip, SavedFileImportsToTheSameModel) {
    const Import original = import_fixture(GetParam());
    ASSERT_FALSE(original.items.empty());
    const Model model = fem::inp::model_of(original);
    // Saved under the name it had: the mesh of a file without parts is named after the file.
    const fs::path file = scratch() / fs::path(GetParam()).filename();
    const std::u8string stem = file.stem().u8string();
    const std::string file_stem(stem.begin(), stem.end());
    const Export first = fem::inp::export_model(model, file_stem);
    EXPECT_GT(first.keywords, 0U);

    write(file, first.text);
    Import again;
    try {
        again = fem::inp::import_file(file, fem::inp::Encoding::utf8);
    } catch (const fem::inp::InpError& error) {
        FAIL() << error.problem().key << " at line " << error.problem().line << ":\n" << first.text;
    }
    Json expected = meaning(original);
    Json actual = meaning(again);
    if (has_loss(first.losses, "loss.sharedPart")) {
        // Instances of one part became parts of their own: the losses say so.
        for (Json* side : {&expected, &actual}) {
            for (auto& [name, mesh] : (*side)["meshes"].items()) {
                mesh["place"].erase("part");
            }
        }
    }
    EXPECT_EQ(expected, actual) << "expected:\n"
                                << expected.dump(1) << "\nactual:\n"
                                << actual.dump(1) << "\nfile:\n"
                                << first.text;

    // A fixed point: the file read again is written to the same bytes, and so is the model.
    EXPECT_EQ(fem::inp::export_model(fem::inp::model_of(again), file_stem).text, first.text);
    EXPECT_EQ(fem::inp::export_model(model, file_stem).text, first.text);
    // describe_losses is the export without the text.
    EXPECT_EQ(loss_keys(fem::inp::describe_losses(model, file_stem)), loss_keys(first.losses));
    EXPECT_FALSE(has_loss(first.losses, "loss.meshName"));
}

INSTANTIATE_TEST_SUITE_P(Fixtures, InpRoundTrip,
                         ::testing::Values("flat.inp", "assembly.inp", "mixed.inp",
                                           "unsupported.inp", "include/main.inp",
                                           "bytes/utf8_bom.inp", "bytes/utf16le.inp",
                                           "bytes/cp1251.inp", "bytes/line_breaks.inp"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string name;
                             for (const char c : std::string(info.param)) {
                                 name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
                             }
                             return name;
                         });

// --- Places of the kept keywords ---------------------------------------------------------------

std::size_t at(const std::string& text, std::string_view what) {
    const std::size_t found = text.find(what);
    EXPECT_NE(found, std::string::npos) << what << " is not in\n" << text;
    return found;
}

TEST(InpExport, KeptKeywordsGoBackToTheirPlaces) {
    const Import result = import_fixture("assembly.inp");
    const std::string text = fem::inp::export_model(fem::inp::model_of(result)).text;
    // Model data before the materials, history data inside its step.
    EXPECT_LT(at(text, "*Preprint, echo=NO"), at(text, "*MATERIAL, NAME=Steel"));
    EXPECT_LT(at(text, "*STEP, NAME=Step-1"), at(text, "*Restart, write, frequency=0"));
    EXPECT_LT(at(text, "*Restart, write, frequency=0"), at(text, "*END STEP"));
    // Each instance is a part of its own, at the coordinates of the assembly.
    EXPECT_NE(text.find("*INSTANCE, NAME=Beam-1, PART=Beam\n*END INSTANCE"), std::string::npos)
        << text;
    EXPECT_NE(text.find("*INSTANCE, NAME=Beam-2, PART=Beam-2\n*END INSTANCE"), std::string::npos)
        << text;
    // The set of the assembly renamed by the import ("Assembly.Set-1") has its name back.
    EXPECT_NE(text.find("*NSET, NSET=Set-1, INSTANCE=Beam-1"), std::string::npos) << text;
}

TEST(InpExport, KeptStepsStayBetweenTheOthers) {
    const Import result = import_fixture("unsupported.inp");
    const std::string text = fem::inp::export_model(fem::inp::model_of(result)).text;
    EXPECT_LT(at(text, "*STEP, NAME=Modes"), at(text, "*STEP, NAME=Static"));
    // *BOUNDARY, OP=NEW comes before the boundary conditions of the step, as in the file.
    EXPECT_LT(at(text, "*BOUNDARY, OP=NEW"), at(text, "ALL, 1, 3\n"));
}

TEST(InpExport, SetsKeepElementsOfTypesTheModelDoesNotHold) {
    // A set of a solid and of a plane element, whose type is kept in passthrough.
    const fs::path file = scratch() / "mixed set.inp";
    write(file,
          "*NODE\n1, 0., 0., 0.\n2, 1., 0., 0.\n3, 0., 1., 0.\n4, 0., 0., 1.\n"
          "*ELEMENT, TYPE=C3D4\n1, 1, 2, 3, 4\n*ELEMENT, TYPE=CPS4\n2, 1, 2, 3, 4\n"
          "*ELSET, ELSET=MIX\n1, 2\n*ELSET, ELSET=PLANE\n2\n");
    const Import original = fem::inp::import_file(file, fem::inp::Encoding::utf8);
    const Export saved = fem::inp::export_model(fem::inp::model_of(original), "mixed set");
    EXPECT_NE(saved.text.find("*ELSET, ELSET=MIX\n1, 2\n"), std::string::npos) << saved.text;
    EXPECT_NE(saved.text.find("*ELSET, ELSET=PLANE\n2\n"), std::string::npos) << saved.text;
    write(file, saved.text);
    EXPECT_EQ(meaning(fem::inp::import_file(file, fem::inp::Encoding::utf8)), meaning(original));
}

TEST(InpExport, ChangedModelIsWritten) {
    Import result = import_fixture("flat.inp");
    for (Item& item : result.items) {
        if (item.type == "material") {
            item.properties["youngsModulus"] = 70000.0;
        }
    }
    const std::string text = fem::inp::export_model(fem::inp::model_of(result)).text;
    EXPECT_NE(text.find("*ELASTIC\n70000., 0.3\n"), std::string::npos) << text;
}

// --- Losses ------------------------------------------------------------------------------------

TEST(InpExport, FileOfAFlatModelLosesOnlyTheUnits) {
    const Import result = import_fixture("flat.inp");
    const Model model = fem::inp::model_of(result);
    const std::vector<Problem> losses = fem::inp::describe_losses(model, "flat");
    EXPECT_EQ(loss_keys(losses), (std::vector<std::string>{"loss.units"}));
    EXPECT_EQ(losses[0].severity, Problem::Severity::info);
    EXPECT_EQ(losses[0].args.at("units"), "m, kg, s, K");
    // Saved under another name, the mesh takes that name.
    EXPECT_EQ(loss_keys(fem::inp::describe_losses(model, "copy")),
              (std::vector<std::string>{"loss.meshName", "loss.units"}));
}

TEST(InpExport, InstancesOfOnePartBecomePartsOfTheirOwn) {
    const Import result = import_fixture("assembly.inp");
    const std::vector<Problem> losses = fem::inp::describe_losses(fem::inp::model_of(result));
    ASSERT_TRUE(has_loss(losses, "loss.sharedPart"));
    EXPECT_FALSE(has_loss(losses, "loss.meshName"));  // Meshes of instances keep their names.
    const auto shared = std::find_if(losses.begin(), losses.end(),
                                     [](const Problem& p) { return p.key == "loss.sharedPart"; });
    EXPECT_EQ(shared->args.at("part"), "Beam");
    EXPECT_EQ(shared->args.at("instance"), "Beam-2");
}

TEST(InpExport, WhatTheFileCannotHoldIsDescribed) {
    Import result = import_fixture("flat.inp");
    // An object of another module, a mesh of pyramids and a load outside the steps.
    Item contact;
    contact.type = "contact_pair";
    contact.owner = "fem.contact";
    contact.name = "Contact-1";
    result.items.push_back(contact);
    fem::inp::MeshData pyramids;
    pyramids.node_ids = {1, 2, 3, 4, 5};
    pyramids.x = {0, 1, 1, 0, 0.5};
    pyramids.y = {0, 0, 1, 1, 0.5};
    pyramids.z = {0, 0, 0, 0, 1};
    pyramids.blocks.push_back({"pyramid5", "", {1}, {0, 1, 2, 3, 4}});
    Item mesh;
    mesh.type = "mesh";
    mesh.owner = "fem.core";
    mesh.name = "pyramids";
    mesh.data = pyramids;
    result.items.push_back(mesh);
    Item load;
    load.type = "gravity";
    load.owner = "fem.model.structural";
    load.name = "Loose";
    load.properties = {{"acceleration", 9.81}};
    result.items.push_back(load);

    const Model model = fem::inp::model_of(result);
    const std::vector<Problem> losses = fem::inp::describe_losses(model);
    for (const char* key :
         {"loss.notExported", "loss.elementType", "loss.outsideStep", "loss.units"}) {
        EXPECT_TRUE(has_loss(losses, key)) << key;
    }
    // Two meshes: the file gets parts and an assembly.
    const std::string text = fem::inp::export_model(model).text;
    EXPECT_NE(text.find("*PART, NAME=flat"), std::string::npos) << text;
    EXPECT_NE(text.find("*PART, NAME=pyramids"), std::string::npos) << text;
    EXPECT_EQ(text.find("pyramid"), text.find("pyramids")) << "no element of the pyramids";
}

// --- Numbers -----------------------------------------------------------------------------------

TEST(InpExport, NumbersAreWrittenShortAndReadBackExactly) {
    EXPECT_EQ(fem::inp::format_number(1.0), "1.");
    EXPECT_EQ(fem::inp::format_number(0.3), "0.3");
    EXPECT_EQ(fem::inp::format_number(-2.5), "-2.5");
    std::mt19937_64 random(42);
    std::uniform_real_distribution<double> mantissa(-1.0, 1.0);
    std::uniform_int_distribution<int> exponent(-300, 300);
    for (int i = 0; i < 10000; ++i) {
        const double value = std::ldexp(mantissa(random), exponent(random));
        EXPECT_EQ(fem::inp::parse_number(fem::inp::format_number(value)), value) << value;
    }
    for (const double value :
         {std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min(),
          std::numeric_limits<double>::max(), 0.1 + 0.2}) {
        EXPECT_EQ(fem::inp::parse_number(fem::inp::format_number(value)), value) << value;
    }
}

}  // namespace
