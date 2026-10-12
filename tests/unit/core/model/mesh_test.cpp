#include "fem/core/model/mesh.h"

#include "fem/core/model/document.h"

#include "model_test_support.h"
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace fem::model;
using fem::model::testing::BusEditor;
using fem::model::testing::make_blocks;
using fem::model::testing::module_a;
using fem::model::testing::module_object;
using fem::model::testing::small_mesh;
using fem::model::testing::tiny_mesh;

namespace {

NodeArrays three_nodes() {
    NodeArrays nodes;
    nodes.ids = {7, 8, 9};
    nodes.x = {0, 1, 0};
    nodes.y = {0, 0, 1};
    nodes.z = {0, 0, 0};
    return nodes;
}

template <class T>
std::vector<T> to_vector(std::span<const T> values) {
    return {values.begin(), values.end()};
}

}  // namespace

TEST(Mesh, ExposesNodesAndElementsInGlobalOrder) {
    const std::shared_ptr<const Mesh> mesh = small_mesh();

    EXPECT_EQ(mesh->node_count(), 5U);
    EXPECT_EQ(to_vector(mesh->node_ids()), (std::vector<Label>{1, 2, 3, 4, 5}));
    EXPECT_EQ(to_vector(mesh->z()), (std::vector<double>{0, 0, 0, 1, -1}));

    ASSERT_EQ(mesh->blocks().size(), 2U);
    EXPECT_EQ(mesh->element_count(), 3U);
    EXPECT_EQ(mesh->block_offset(0), 0U);
    EXPECT_EQ(mesh->block_offset(1), 2U);
    EXPECT_EQ(mesh->block_offset(2), 3U);

    const ElementBlock& tets = mesh->blocks()[0];
    EXPECT_EQ(tets.type(), ElementType::tet4);
    EXPECT_EQ(tets.nodes_per_element(), 4);
    EXPECT_EQ(tets.size(), 2U);
    EXPECT_EQ(to_vector(tets.ids()), (std::vector<Label>{10, 11}));
    EXPECT_EQ(to_vector(tets.element_nodes(1)), (std::vector<Index>{0, 2, 1, 4}));

    const Mesh::ElementLocation location = mesh->locate(2);
    EXPECT_EQ(location.block, 1U);
    EXPECT_EQ(location.element, 0U);
    EXPECT_EQ(mesh->element_type(1), ElementType::tet4);
    EXPECT_EQ(mesh->element_type(2), ElementType::tri3);
    EXPECT_MODEL_ERROR((void)mesh->locate(3), ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR((void)mesh->locate(-1), ModelErrc::invalid_data);
}

TEST(Mesh, EmptyMeshIsValid) {
    const Mesh mesh(NodeArrays{}, {});
    EXPECT_EQ(mesh.node_count(), 0U);
    EXPECT_EQ(mesh.element_count(), 0U);
    EXPECT_TRUE(mesh.blocks().empty());
}

TEST(Mesh, TakesArraysWithoutCopying) {
    NodeArrays nodes = three_nodes();
    const double* x = nodes.x.data();
    const double* y = nodes.y.data();
    const double* z = nodes.z.data();
    const Label* node_ids = nodes.ids.data();
    std::vector<Label> element_ids = {1};
    std::vector<Index> connectivity = {0, 1, 2};
    const Index* conn = connectivity.data();
    const Label* elem_ids = element_ids.data();

    Mesh mesh(std::move(nodes), make_blocks(ElementBlock(ElementType::tri3, std::move(element_ids),
                                                         std::move(connectivity))));
    auto shared = std::make_shared<const Mesh>(std::move(mesh));

    Document doc;
    BusEditor edit{doc};
    const ObjectId id = edit.create(mesh_object("part", shared));
    shared.reset();

    const std::shared_ptr<const Mesh> stored = doc.data<Mesh>(id);
    EXPECT_EQ(stored->x().data(), x);
    EXPECT_EQ(stored->y().data(), y);
    EXPECT_EQ(stored->z().data(), z);
    EXPECT_EQ(stored->node_ids().data(), node_ids);
    EXPECT_EQ(stored->blocks()[0].connectivity().data(), conn);
    EXPECT_EQ(stored->blocks()[0].ids().data(), elem_ids);
}

TEST(Mesh, RejectsInconsistentNodeArrays) {
    NodeArrays nodes = three_nodes();
    nodes.y.pop_back();
    EXPECT_MODEL_ERROR(Mesh(std::move(nodes), {}), ModelErrc::invalid_mesh);

    NodeArrays labels = three_nodes();
    labels.ids.push_back(10);
    EXPECT_MODEL_ERROR(Mesh(std::move(labels), {}), ModelErrc::invalid_mesh);
}

TEST(Mesh, RejectsNonFiniteCoordinates) {
    NodeArrays nodes = three_nodes();
    nodes.z[1] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_MODEL_ERROR(Mesh(std::move(nodes), {}), ModelErrc::invalid_mesh);

    NodeArrays infinite = three_nodes();
    infinite.x[0] = std::numeric_limits<double>::infinity();
    EXPECT_MODEL_ERROR(Mesh(std::move(infinite), {}), ModelErrc::invalid_mesh);
}

TEST(Mesh, RejectsConnectivityOfWrongSize) {
    EXPECT_MODEL_ERROR(ElementBlock(ElementType::tri3, {1, 2}, {0, 1, 2}), ModelErrc::invalid_mesh);
    EXPECT_MODEL_ERROR(ElementBlock(ElementType::quad4, {1}, {0, 1, 2}), ModelErrc::invalid_mesh);
}

TEST(Mesh, RejectsConnectivityOutsideTheNodes) {
    EXPECT_MODEL_ERROR(
        Mesh(three_nodes(), make_blocks(ElementBlock(ElementType::tri3, {1}, {0, 1, 3}))),
        ModelErrc::invalid_mesh);
    EXPECT_MODEL_ERROR(
        Mesh(three_nodes(), make_blocks(ElementBlock(ElementType::line2, {1}, {-1, 1}))),
        ModelErrc::invalid_mesh);
}

TEST(MeshSets, SortAndDeduplicateInPlace) {
    std::vector<Index> indices = {4, 1, 3, 1, 0};
    const Index* storage = indices.data();
    const NodeSet nodes(std::move(indices));
    EXPECT_EQ(to_vector(nodes.nodes()), (std::vector<Index>{0, 1, 3, 4}));
    EXPECT_EQ(nodes.nodes().data(), storage);
    EXPECT_EQ(nodes.size(), 4U);

    const ElementSet elements({2, 0, 2});
    EXPECT_EQ(to_vector(elements.elements()), (std::vector<Index>{0, 2}));

    const Surface surface({{1, 2}, {0, 3}, {1, 0}, {1, 2}});
    EXPECT_EQ(to_vector(surface.sides()), (std::vector<ElementSide>{{0, 3}, {1, 0}, {1, 2}}));
}

TEST(MeshSets, RejectNegativeIndices) {
    EXPECT_MODEL_ERROR(NodeSet({0, -1}), ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR(ElementSet({-5}), ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR(Surface({{0, -1}}), ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR(Surface({{-1, 0}}), ModelErrc::invalid_data);
}

TEST(MeshSets, AreCheckedAgainstTheMesh) {
    const std::shared_ptr<const Mesh> mesh = small_mesh();

    NodeSet({0, 4}).check_against(*mesh);
    EXPECT_MODEL_ERROR(NodeSet({5}).check_against(*mesh), ModelErrc::invalid_data);

    ElementSet({0, 2}).check_against(*mesh);
    EXPECT_MODEL_ERROR(ElementSet({3}).check_against(*mesh), ModelErrc::invalid_data);

    // Element 1 is a tet (4 sides), element 2 a triangle (3 sides).
    Surface({{1, 3}, {2, 2}}).check_against(*mesh);
    EXPECT_MODEL_ERROR(Surface({{2, 3}}).check_against(*mesh), ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR(Surface({{3, 0}}).check_against(*mesh), ModelErrc::invalid_data);
}

TEST(MeshObjects, SetsLiveUnderTheirMesh) {
    Document doc;
    BusEditor edit{doc};
    const ObjectId mesh = edit.create(mesh_object("part", small_mesh()));
    const ObjectId nodes = edit.create(node_set_object(
        mesh, "fixed", std::make_shared<const NodeSet>(std::vector<Index>{0, 1, 2})));
    const ObjectId elements = edit.create(element_set_object(
        mesh, "solid", std::make_shared<const ElementSet>(std::vector<Index>{0, 1})));
    const ObjectId surface = edit.create(surface_object(
        mesh, "pressure", std::make_shared<const Surface>(std::vector<ElementSide>{{0, 1}})));

    const Object& mesh_object_ref = doc.get(mesh);
    EXPECT_EQ(mesh_object_ref.type(), core_types::mesh);
    EXPECT_EQ(mesh_object_ref.owner(), core_owner);
    EXPECT_EQ(doc.get(nodes).type(), core_types::node_set);
    EXPECT_EQ(doc.get(elements).type(), core_types::element_set);
    EXPECT_EQ(doc.get(surface).type(), core_types::surface);
    EXPECT_EQ(doc.get(surface).parent(), mesh);
    EXPECT_EQ(doc.find_child(mesh, core_types::node_set, "fixed"), nodes);

    EXPECT_EQ(doc.data<NodeSet>(nodes)->size(), 3U);
    EXPECT_EQ(doc.data<Mesh>(mesh)->node_count(), 5U);
    EXPECT_MODEL_ERROR((void)doc.data<Mesh>(nodes), ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR((void)doc.data<NodeSet>(elements), ModelErrc::invalid_data);
}

TEST(MeshObjects, SetsNeedAMeshParentAndMatchingIndices) {
    Document doc;
    BusEditor edit{doc};
    doc.set_owner_enabled(module_a, true);
    const ObjectId mesh = edit.create(mesh_object("part", small_mesh()));
    const ObjectId group = edit.create(module_object(module_a, "group", "g"));
    const auto set = std::make_shared<const NodeSet>(std::vector<Index>{0});

    EXPECT_MODEL_ERROR(edit.create(node_set_object({}, "orphan", set)), ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR(edit.create(node_set_object(group, "misplaced", set)),
                       ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR(
        edit.create(node_set_object(mesh, "outside",
                                    std::make_shared<const NodeSet>(std::vector<Index>{5}))),
        ModelErrc::invalid_data);
    EXPECT_EQ(doc.size(), 2U);
}

TEST(MeshObjects, CoreTypesRequireTheirData) {
    Document doc;
    BusEditor edit{doc};
    EXPECT_MODEL_ERROR(edit.create(mesh_object("empty", nullptr)), ModelErrc::invalid_data);

    NewObject wrong = mesh_object("wrong", small_mesh());
    wrong.data = std::make_shared<const NodeSet>(std::vector<Index>{0});
    EXPECT_MODEL_ERROR(edit.create(wrong), ModelErrc::invalid_data);

    const ObjectId mesh = edit.create(mesh_object("part", small_mesh()));
    NewObject wrong_set =
        element_set_object(mesh, "s", std::make_shared<const ElementSet>(std::vector<Index>{0}));
    wrong_set.type = std::string(core_types::node_set);
    EXPECT_MODEL_ERROR(edit.create(wrong_set), ModelErrc::invalid_data);
}

TEST(MeshObjects, ReplacingTheMeshKeepsSetsValid) {
    Document doc;
    BusEditor edit{doc};
    const ObjectId mesh = edit.create(mesh_object("part", small_mesh()));
    const ObjectId set = edit.create(
        node_set_object(mesh, "s", std::make_shared<const NodeSet>(std::vector<Index>{1})));
    const auto original = doc.data<Mesh>(mesh);

    // The set refers to node 1, which a two-node mesh still has...
    edit.set_data(mesh, tiny_mesh());
    EXPECT_EQ(doc.data<Mesh>(mesh)->node_count(), 2U);

    // ...but not to element sides of a mesh without elements.
    edit.create(
        surface_object(mesh, "faces", std::make_shared<const Surface>(std::vector<ElementSide>{})));
    edit.set_data(mesh, original);
    edit.set_data(doc.find_child(mesh, core_types::surface, "faces"),
                  std::make_shared<const Surface>(std::vector<ElementSide>{{2, 0}}));
    EXPECT_MODEL_ERROR(edit.set_data(mesh, tiny_mesh()), ModelErrc::invalid_data);
    EXPECT_EQ(doc.data<Mesh>(mesh), original);

    EXPECT_MODEL_ERROR(edit.set_data(mesh, nullptr), ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR(edit.set_data(mesh, std::make_shared<const NodeSet>(std::vector<Index>{0})),
                       ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR(
        edit.set_data(set, std::make_shared<const ElementSet>(std::vector<Index>{0})),
        ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR(edit.set_data(set, std::make_shared<const NodeSet>(std::vector<Index>{9})),
                       ModelErrc::invalid_data);

    edit.set_data(set, std::make_shared<const NodeSet>(std::vector<Index>{3, 4}));
    EXPECT_EQ(doc.data<NodeSet>(set)->size(), 2U);
}
