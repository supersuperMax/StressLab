#include "fem/core/model/document.h"

#include "model_test_support.h"
#include <gtest/gtest.h>

#include <vector>

using namespace fem::model;
using fem::model::testing::module_a;
using fem::model::testing::module_object;

namespace {

std::vector<ObjectId> dependencies_of(const Document& doc, ObjectId id) {
    const auto deps = doc.get(id).dependencies();
    return {deps.begin(), deps.end()};
}

class DependencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        doc.set_owner_enabled(module_a, true);
        a = doc.create(module_object(module_a, "item", "a"));
        b = doc.create(module_object(module_a, "item", "b"));
        c = doc.create(module_object(module_a, "item", "c"));
    }

    Document doc;
    ObjectId a, b, c;
};

}  // namespace

TEST_F(DependencyTest, AddRecordsBothDirections) {
    EXPECT_TRUE(doc.add_dependency(a, c));
    EXPECT_TRUE(doc.add_dependency(a, b));
    EXPECT_TRUE(doc.add_dependency(b, c));

    EXPECT_EQ(dependencies_of(doc, a), (std::vector<ObjectId>{b, c}));
    EXPECT_TRUE(doc.get(a).depends_on(c));
    EXPECT_FALSE(doc.get(c).depends_on(a));
    EXPECT_EQ(doc.dependents(c), (std::vector<ObjectId>{a, b}));
    EXPECT_EQ(doc.dependents(b), std::vector<ObjectId>{a});
    EXPECT_TRUE(doc.dependents(a).empty());
}

TEST_F(DependencyTest, AddIsIdempotent) {
    EXPECT_TRUE(doc.add_dependency(a, b));
    EXPECT_FALSE(doc.add_dependency(a, b));
    EXPECT_EQ(dependencies_of(doc, a), std::vector<ObjectId>{b});
}

TEST_F(DependencyTest, CreateAcceptsDependenciesSortedAndUnique) {
    const ObjectId d = doc.create(module_object(module_a, "item", "d", {}, {c, a, c}));
    EXPECT_EQ(dependencies_of(doc, d), (std::vector<ObjectId>{a, c}));
    EXPECT_EQ(doc.dependents(a), std::vector<ObjectId>{d});
    EXPECT_EQ(doc.dependents(c), std::vector<ObjectId>{d});
}

TEST_F(DependencyTest, RejectsSelfAndMissingTargets) {
    EXPECT_MODEL_ERROR(doc.add_dependency(a, a), ModelErrc::invalid_dependency);
    EXPECT_MODEL_ERROR(doc.add_dependency(a, ObjectId{99}), ModelErrc::invalid_dependency);
    EXPECT_MODEL_ERROR(doc.add_dependency(a, ObjectId{}), ModelErrc::invalid_dependency);
    EXPECT_MODEL_ERROR(doc.add_dependency(ObjectId{99}, a), ModelErrc::unknown_object);
    EXPECT_MODEL_ERROR(doc.create(module_object(module_a, "item", "d", {}, {ObjectId{99}})),
                       ModelErrc::invalid_dependency);
    EXPECT_TRUE(doc.get(a).dependencies().empty());
    EXPECT_EQ(doc.size(), 3U);
}

TEST_F(DependencyTest, RejectsCycles) {
    doc.add_dependency(a, b);
    doc.add_dependency(b, c);

    EXPECT_MODEL_ERROR(doc.add_dependency(c, a), ModelErrc::dependency_cycle);
    EXPECT_MODEL_ERROR(doc.add_dependency(b, a), ModelErrc::dependency_cycle);
    EXPECT_TRUE(doc.get(c).dependencies().empty());
    EXPECT_TRUE(doc.dependents(a).empty());

    // A diamond is not a cycle.
    EXPECT_TRUE(doc.add_dependency(a, c));
}

TEST_F(DependencyTest, RemoveDependencyUpdatesBothDirections) {
    doc.add_dependency(a, b);
    doc.add_dependency(a, c);

    EXPECT_TRUE(doc.remove_dependency(a, b));
    EXPECT_FALSE(doc.remove_dependency(a, b));
    EXPECT_EQ(dependencies_of(doc, a), std::vector<ObjectId>{c});
    EXPECT_TRUE(doc.dependents(b).empty());
    EXPECT_EQ(doc.dependents(c), std::vector<ObjectId>{a});
}

// An active module shows such a dependency as broken and lets the user fix it; ids are never
// reused, so the reference stays unambiguous, and undo can bring the target back.
TEST_F(DependencyTest, RemovingTargetLeavesMissingDependencyOnActiveObject) {
    doc.add_dependency(a, c);
    doc.add_dependency(b, c);

    const RemoveResult result = doc.remove(c);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.removed, std::vector<ObjectId>{c});

    EXPECT_EQ(dependencies_of(doc, a), std::vector<ObjectId>{c});
    EXPECT_EQ(doc.missing_dependencies(a), std::vector<ObjectId>{c});
    EXPECT_EQ(doc.dependents(c), (std::vector<ObjectId>{a, b}));

    // The broken dependency can be dropped, but not added again.
    EXPECT_TRUE(doc.remove_dependency(a, c));
    EXPECT_EQ(doc.dependents(c), std::vector<ObjectId>{b});
    EXPECT_MODEL_ERROR(doc.add_dependency(a, c), ModelErrc::invalid_dependency);
}

TEST_F(DependencyTest, RemovingDependentCleansReverseIndex) {
    doc.add_dependency(a, c);
    ASSERT_TRUE(doc.remove(a).ok());
    EXPECT_TRUE(doc.dependents(c).empty());
}

TEST_F(DependencyTest, DependenciesInsideRemovedSubtreeDoNotBlock) {
    const ObjectId group = doc.create(module_object(module_a, "group", "g"));
    const ObjectId x = doc.create(module_object(module_a, "item", "x", group));
    const ObjectId y = doc.create(module_object(module_a, "item", "y", group, {x}));

    const RemoveResult result = doc.remove(group);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.removed, (std::vector<ObjectId>{group, x, y}));
    EXPECT_TRUE(doc.dependents(x).empty());
}

TEST_F(DependencyTest, MissingDependenciesOfIntactObjectIsEmpty) {
    doc.add_dependency(a, b);
    EXPECT_TRUE(doc.missing_dependencies(a).empty());
    EXPECT_MODEL_ERROR((void)doc.missing_dependencies(ObjectId{99}), ModelErrc::unknown_object);
}
