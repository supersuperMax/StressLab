// Objects of disabled modules (docs/architecture.md §4.6, item 4): kept unchanged, not editable,
// and nothing they need can be removed.

#include "fem/core/model/document.h"

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
using Reason = RemovalBlocker::Reason;

namespace {

// A mesh with a node set, an active load on the set (module A) and a "dynamics" object
// (module B) that depends on the set too; module B is then disabled.
class OpaqueTest : public ::testing::Test {
protected:
    void SetUp() override {
        doc.set_owner_enabled(module_a, true);
        doc.set_owner_enabled(module_b, true);
        mesh = edit.create(mesh_object("part", small_mesh()));
        set = edit.create(node_set_object(
            mesh, "fixed", std::make_shared<const NodeSet>(std::vector<Index>{0, 1})));
        load = edit.create(module_object(module_a, "load", "p", {}, {set}));

        NewObject damping = module_object(module_b, "damping", "rayleigh", {}, {set});
        damping.schema_version = 2;
        damping.properties = {{"alpha", 0.1}, {"beta", 1e-5}};
        opaque = edit.create(damping);
        doc.set_owner_enabled(module_b, false);
    }

    Document doc;
    BusEditor edit{doc};
    ObjectId mesh, set, load, opaque;
};

}  // namespace

TEST_F(OpaqueTest, ObjectsOfDisabledModulesAreOpaque) {
    EXPECT_TRUE(doc.is_opaque(opaque));
    EXPECT_FALSE(doc.is_opaque(load));
    EXPECT_FALSE(doc.is_opaque(mesh));
    EXPECT_FALSE(doc.is_opaque(set));

    doc.set_owner_enabled(module_a, false);
    EXPECT_TRUE(doc.is_opaque(load));
    EXPECT_FALSE(doc.is_opaque(mesh));  // The core is never opaque.

    doc.set_owner_enabled(module_b, true);
    EXPECT_FALSE(doc.is_opaque(opaque));
    EXPECT_MODEL_ERROR((void)doc.is_opaque(ObjectId{99}), ModelErrc::unknown_object);
}

TEST(Opaque, ObjectsOfUnknownModulesAreOpaque) {
    // A model loaded with objects of a module that is not installed.
    Document doc;
    BusEditor edit{doc};
    const ObjectId id = edit.create(module_object("vendor.unknown", "thing", "t"));
    EXPECT_TRUE(doc.is_opaque(id));
    EXPECT_EQ(doc.owners(), std::vector<std::string>{"vendor.unknown"});
}

TEST_F(OpaqueTest, OpaqueObjectIsKeptUnchanged) {
    const Object& object = doc.get(opaque);
    EXPECT_EQ(object.type(), "damping");
    EXPECT_EQ(object.owner(), module_b);
    EXPECT_EQ(object.schema_version(), 2U);
    EXPECT_EQ(object.name(), "rayleigh");
    EXPECT_EQ(object.properties(), (Properties{{"alpha", 0.1}, {"beta", 1e-5}}));
    EXPECT_TRUE(object.depends_on(set));
    EXPECT_EQ(doc.owners(),
              (std::vector<std::string>{std::string(core_owner), module_a, module_b}));

    doc.set_owner_enabled(module_b, true);
    EXPECT_EQ(doc.get(opaque).properties(), (Properties{{"alpha", 0.1}, {"beta", 1e-5}}));
}

TEST_F(OpaqueTest, OpaqueObjectCannotBeEdited) {
    EXPECT_MODEL_ERROR(edit.rename(opaque, "other"), ModelErrc::opaque_object);
    EXPECT_MODEL_ERROR(edit.set_properties(opaque, {{"alpha", 0.2}}), ModelErrc::opaque_object);
    EXPECT_MODEL_ERROR(edit.set_data(opaque, nullptr), ModelErrc::opaque_object);
    EXPECT_MODEL_ERROR(edit.add_dependency(opaque, load), ModelErrc::opaque_object);
    EXPECT_MODEL_ERROR(edit.remove_dependency(opaque, set), ModelErrc::opaque_object);
    EXPECT_MODEL_ERROR(edit.create(module_object(module_a, "note", "n", opaque)),
                       ModelErrc::opaque_object);

    EXPECT_EQ(doc.get(opaque).name(), "rayleigh");
    EXPECT_TRUE(doc.get(opaque).depends_on(set));
    EXPECT_TRUE(doc.get(opaque).children().empty());

    doc.set_owner_enabled(module_b, true);
    edit.rename(opaque, "other");
    EXPECT_EQ(doc.get(opaque).name(), "other");
}

TEST_F(OpaqueTest, ActiveObjectsMayDependOnOpaqueOnes) {
    EXPECT_TRUE(edit.add_dependency(load, opaque));
    EXPECT_TRUE(edit.remove_dependency(load, opaque));
}

TEST_F(OpaqueTest, OpaqueObjectCannotBeRemoved) {
    const std::vector<RemovalBlocker> expected = {{Reason::opaque, opaque, opaque}};
    EXPECT_EQ(doc.removal_blockers(opaque), expected);

    const RemoveResult result = edit.remove(opaque);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.removed.empty());
    EXPECT_EQ(result.blockers, expected);
    EXPECT_TRUE(doc.contains(opaque));
}

TEST_F(OpaqueTest, ObjectRequiredByOpaqueObjectCannotBeRemoved) {
    const std::vector<RemovalBlocker> expected = {{Reason::required_by_opaque, set, opaque}};
    EXPECT_EQ(doc.removal_blockers(set), expected);

    const RemoveResult result = edit.remove(set);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.blockers, expected);
    EXPECT_TRUE(doc.contains(set));
    EXPECT_EQ(doc.dependents(set), (std::vector<ObjectId>{load, opaque}));
    EXPECT_TRUE(doc.missing_dependencies(opaque).empty());
}

TEST_F(OpaqueTest, AncestorOfRequiredObjectCannotBeRemoved) {
    // Removing the mesh would take the set with it.
    const RemoveResult result = edit.remove(mesh);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.blockers,
              (std::vector<RemovalBlocker>{{Reason::required_by_opaque, set, opaque}}));
    EXPECT_EQ(doc.size(), 4U);
    EXPECT_EQ(doc.get(mesh).children().size(), 1U);
}

TEST_F(OpaqueTest, AncestorOfOpaqueObjectCannotBeRemoved) {
    doc.set_owner_enabled(module_b, true);
    const ObjectId step = edit.create(module_object(module_a, "step", "s1"));
    const ObjectId child = edit.create(module_object(module_b, "damping", "local", step));
    doc.set_owner_enabled(module_b, false);

    const RemoveResult result = edit.remove(step);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.blockers, (std::vector<RemovalBlocker>{{Reason::opaque, child, child}}));
    EXPECT_TRUE(doc.contains(step));
    EXPECT_TRUE(doc.contains(child));
}

TEST_F(OpaqueTest, ActiveDependentsDoNotBlockRemoval) {
    // The active load depends on the set as well; once the opaque object is gone, the set can go.
    doc.set_owner_enabled(module_b, true);
    ASSERT_TRUE(edit.remove(opaque).ok());

    const RemoveResult result = edit.remove(set);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(doc.missing_dependencies(load), std::vector<ObjectId>{set});
}

TEST_F(OpaqueTest, EnablingTheModuleLiftsTheRestrictions) {
    doc.set_owner_enabled(module_b, true);
    EXPECT_TRUE(doc.removal_blockers(set).empty());
    ASSERT_TRUE(edit.remove(mesh).ok());
    EXPECT_EQ(doc.missing_dependencies(opaque), std::vector<ObjectId>{set});
}

TEST_F(OpaqueTest, AllBlockersAreReportedInOrder) {
    doc.set_owner_enabled(module_b, true);
    const ObjectId other_set = edit.create(
        node_set_object(mesh, "top", std::make_shared<const NodeSet>(std::vector<Index>{3})));
    const ObjectId second =
        edit.create(module_object(module_b, "damping", "second", {}, {other_set}));
    const ObjectId inside = edit.create(module_object(module_b, "marker", "m", mesh));
    doc.set_owner_enabled(module_b, false);

    const std::vector<RemovalBlocker> expected = {
        {Reason::required_by_opaque, set, opaque},
        {Reason::required_by_opaque, other_set, second},
        {Reason::opaque, inside, inside},
    };
    EXPECT_EQ(doc.removal_blockers(mesh), expected);
    EXPECT_EQ(edit.remove(mesh).blockers, expected);
}

TEST_F(OpaqueTest, OpaqueObjectInsideRemovedSubtreeIsReportedOnce) {
    // An opaque object that depends on something in the same subtree would be lost, not broken.
    doc.set_owner_enabled(module_b, true);
    const ObjectId group = edit.create(module_object(module_a, "group", "g"));
    const ObjectId target = edit.create(module_object(module_a, "item", "t", group));
    const ObjectId inner = edit.create(module_object(module_b, "damping", "d", group, {target}));
    doc.set_owner_enabled(module_b, false);

    EXPECT_EQ(doc.removal_blockers(group),
              (std::vector<RemovalBlocker>{{Reason::opaque, inner, inner}}));
}
