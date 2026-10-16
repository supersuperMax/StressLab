// Putting back a removed object with its id (DocumentEditor::restore): restoring one object from an
// older version of the project (task 2.3).

#include "fem/core/model/document.h"
#include "fem/core/model/mesh.h"

#include "model_test_support.h"
#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace fem::model;
using fem::model::testing::BusEditor;
using fem::model::testing::module_a;
using fem::model::testing::module_b;
using fem::model::testing::module_object;
using fem::model::testing::small_mesh;
using fem::model::testing::stored_copy;

namespace {

std::vector<ObjectId> children_of(const Document& doc, ObjectId parent) {
    if (!parent.is_valid()) {
        return {doc.roots().begin(), doc.roots().end()};
    }
    const auto children = doc.get(parent).children();
    return {children.begin(), children.end()};
}

class RestoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        doc.set_owner_enabled(module_a, true);
        doc.set_owner_enabled(module_b, true);
    }

    Document doc;
    BusEditor edit{doc};
};

}  // namespace

TEST_F(RestoreTest, RemovedObjectComesBackWithItsIdAndPlace) {
    const ObjectId a = edit.create(module_object(module_a, "item", "a"));
    NewObject init = module_object(module_a, "item", "b");
    init.properties = Properties{{"thickness", 2.0}};
    const ObjectId b = edit.create(std::move(init));
    const ObjectId c = edit.create(module_object(module_a, "item", "c"));
    const StoredObject saved = stored_copy(doc, b);
    (void)edit.remove(b);
    const std::uint64_t next = doc.next_id();

    edit.restore(saved);

    EXPECT_EQ(children_of(doc, {}), (std::vector<ObjectId>{a, b, c}));
    EXPECT_EQ(doc.find_child({}, "item", "b"), b);
    EXPECT_EQ(doc.get(b).properties(), (Properties{{"thickness", 2.0}}));
    EXPECT_EQ(doc.next_id(), next);
}

TEST_F(RestoreTest, ChildTakesItsPlaceAmongSiblingsById) {
    const ObjectId parent = edit.create(module_object(module_a, "group", "g"));
    const ObjectId first = edit.create(module_object(module_a, "item", "1", parent));
    const ObjectId second = edit.create(module_object(module_a, "item", "2", parent));
    const ObjectId third = edit.create(module_object(module_a, "item", "3", parent));
    const StoredObject saved = stored_copy(doc, second);
    (void)edit.remove(second);

    edit.restore(saved);

    EXPECT_EQ(children_of(doc, parent), (std::vector<ObjectId>{first, second, third}));
}

TEST_F(RestoreTest, DependentsSeeTheObjectAgain) {
    const ObjectId material = edit.create(module_object(module_a, "material", "steel"));
    const ObjectId section =
        edit.create(module_object(module_a, "section", "skin", {}, {material}));
    const StoredObject saved = stored_copy(doc, material);
    (void)edit.remove(material);
    ASSERT_EQ(doc.missing_dependencies(section), std::vector<ObjectId>{material});

    edit.restore(saved);

    EXPECT_TRUE(doc.missing_dependencies(section).empty());
    EXPECT_EQ(doc.dependents(material), std::vector<ObjectId>{section});
}

TEST_F(RestoreTest, RestoreIsOneStepOfUndoAndRedo) {
    const ObjectId a = edit.create(module_object(module_a, "item", "a"));
    const StoredObject saved = stored_copy(doc, a);
    (void)edit.remove(a);

    edit.restore(saved);
    ASSERT_TRUE(doc.contains(a));
    ASSERT_TRUE(edit.bus().undo());
    EXPECT_FALSE(doc.contains(a));
    EXPECT_FALSE(doc.find_child({}, "item", "a").is_valid());
    ASSERT_TRUE(edit.bus().redo());
    EXPECT_TRUE(doc.contains(a));
    EXPECT_EQ(doc.find_child({}, "item", "a"), a);
}

TEST_F(RestoreTest, MeshAndItsSetComeBackWithTheirData) {
    const ObjectId mesh = edit.create(mesh_object("wing", small_mesh()));
    const ObjectId set = edit.create(
        node_set_object(mesh, "fixed", std::make_shared<const NodeSet>(std::vector<Index>{0, 1})));
    const StoredObject saved_mesh = stored_copy(doc, mesh);
    const StoredObject saved_set = stored_copy(doc, set);
    (void)edit.remove(mesh);

    edit.restore(saved_mesh);
    edit.restore(saved_set);

    EXPECT_EQ(doc.data<Mesh>(mesh)->node_count(), 5U);
    EXPECT_EQ(doc.data<NodeSet>(set)->size(), 2U);
    EXPECT_EQ(children_of(doc, mesh), std::vector<ObjectId>{set});
}

TEST_F(RestoreTest, OnlyRemovedObjectsWithIssuedIdsCanBeRestored) {
    const ObjectId a = edit.create(module_object(module_a, "item", "a"));
    StoredObject present = stored_copy(doc, a);
    EXPECT_MODEL_ERROR(edit.restore(present), ModelErrc::invalid_argument);

    StoredObject never{ObjectId(doc.next_id()), module_object(module_a, "item", "x")};
    EXPECT_MODEL_ERROR(edit.restore(never), ModelErrc::invalid_argument);
    StoredObject zero{ObjectId{}, module_object(module_a, "item", "x")};
    EXPECT_MODEL_ERROR(edit.restore(zero), ModelErrc::invalid_argument);
}

TEST_F(RestoreTest, ItIsCheckedAsCreateIs) {
    const ObjectId parent = edit.create(module_object(module_a, "group", "g"));
    const ObjectId child = edit.create(module_object(module_a, "item", "c", parent));
    const ObjectId other = edit.create(module_object(module_a, "item", "o"));
    const StoredObject saved_child = stored_copy(doc, child);
    StoredObject with_dependency = stored_copy(doc, other);
    with_dependency.init.dependencies = {child};
    (void)edit.remove(parent);
    (void)edit.remove(other);

    // The parent is gone.
    EXPECT_MODEL_ERROR(edit.restore(saved_child), ModelErrc::unknown_object);
    // The dependency is gone.
    EXPECT_MODEL_ERROR(edit.restore(with_dependency), ModelErrc::invalid_dependency);
    // The name is taken by a newer sibling.
    (void)edit.create(module_object(module_a, "item", "o"));
    StoredObject named = stored_copy(doc, doc.find_child({}, "item", "o"));
    named.id = other;
    EXPECT_MODEL_ERROR(edit.restore(named), ModelErrc::duplicate_name);
    EXPECT_FALSE(doc.contains(other));
}

TEST_F(RestoreTest, CycleThroughAKeptDependencyIsRefused) {
    const ObjectId a = edit.create(module_object(module_a, "item", "a"));
    const ObjectId b = edit.create(module_object(module_a, "item", "b", {}, {a}));
    StoredObject saved = stored_copy(doc, a);
    (void)edit.remove(a);  // b keeps its dependency on a, now missing.
    saved.init.dependencies = {b};

    EXPECT_MODEL_ERROR(edit.restore(saved), ModelErrc::dependency_cycle);
    EXPECT_FALSE(doc.contains(a));
}

TEST_F(RestoreTest, NothingGoesUnderAnObjectOfADisabledModule) {
    const ObjectId parent = edit.create(module_object(module_b, "group", "g"));
    const ObjectId child = edit.create(module_object(module_a, "item", "c", parent));
    const StoredObject saved = stored_copy(doc, child);
    (void)edit.remove(child);
    doc.set_owner_enabled(module_b, false);

    EXPECT_MODEL_ERROR(edit.restore(saved), ModelErrc::opaque_object);
}
