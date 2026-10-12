#include "fem/core/model/document.h"

#include "model_test_support.h"
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace fem::model;
using fem::model::testing::module_a;
using fem::model::testing::module_b;
using fem::model::testing::module_object;

namespace {

std::vector<ObjectId> ids_of(const Document& doc) {
    std::vector<ObjectId> result;
    for (const Object& object : doc.objects()) {
        result.push_back(object.id());
    }
    return result;
}

// Text as UTF-8 bytes. GoogleTest from vcpkg is built as C++17 and cannot print char8_t strings.
std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

class DocumentTest : public ::testing::Test {
protected:
    void SetUp() override {
        doc.set_owner_enabled(module_a, true);
        doc.set_owner_enabled(module_b, true);
    }

    Document doc;
};

}  // namespace

TEST(Document, NewDocumentIsEmptyAndDeclaresSiUnits) {
    const Document doc;
    EXPECT_EQ(doc.size(), 0U);
    EXPECT_TRUE(doc.roots().empty());
    EXPECT_EQ(doc.units(), UnitSystem::si());
    EXPECT_TRUE(doc.owners().empty());
}

TEST(Document, UnitSystemIsExplicitAndCanBeChanged) {
    Document doc(UnitSystem::mm_t_s());
    EXPECT_EQ(doc.units(), UnitSystem::mm_t_s());
    doc.set_units(UnitSystem::in_slinch_s());
    EXPECT_EQ(doc.units(), UnitSystem::in_slinch_s());
}

TEST_F(DocumentTest, CreateStoresAllFields) {
    NewObject init = module_object(module_a, "material", "steel");
    init.schema_version = 3;
    init.properties = {{"young", 2.1e11}, {"comment", "rolled"}};

    const ObjectId id = doc.create(init);

    const Object& object = doc.get(id);
    EXPECT_EQ(object.id(), id);
    EXPECT_EQ(object.type(), "material");
    EXPECT_EQ(object.owner(), module_a);
    EXPECT_EQ(object.schema_version(), 3U);
    EXPECT_EQ(object.name(), "steel");
    EXPECT_EQ(object.properties(), init.properties);
    EXPECT_FALSE(object.parent().is_valid());
    EXPECT_TRUE(object.children().empty());
    EXPECT_TRUE(object.dependencies().empty());
    EXPECT_EQ(object.data(), nullptr);
}

TEST_F(DocumentTest, IdsIncreaseAndAreNeverReused) {
    const ObjectId a = doc.create(module_object(module_a, "item", "a"));
    const ObjectId b = doc.create(module_object(module_a, "item", "b"));
    EXPECT_EQ(a, ObjectId{1});
    EXPECT_EQ(b, ObjectId{2});

    ASSERT_TRUE(doc.remove(b).ok());
    const ObjectId c = doc.create(module_object(module_a, "item", "b"));
    EXPECT_EQ(c, ObjectId{3});
}

TEST_F(DocumentTest, FailedCreateDoesNotConsumeAnId) {
    NewObject bad = module_object(module_a, "item");
    bad.properties = 42;
    EXPECT_MODEL_ERROR(doc.create(bad), ModelErrc::invalid_properties);
    EXPECT_EQ(doc.size(), 0U);
    EXPECT_EQ(doc.create(module_object(module_a, "item")), ObjectId{1});
}

TEST_F(DocumentTest, FindAndGet) {
    const ObjectId id = doc.create(module_object(module_a, "item"));
    ASSERT_NE(doc.find(id), nullptr);
    EXPECT_EQ(doc.find(id)->id(), id);
    EXPECT_TRUE(doc.contains(id));

    EXPECT_EQ(doc.find(ObjectId{99}), nullptr);
    EXPECT_FALSE(doc.contains(ObjectId{99}));
    EXPECT_FALSE(doc.contains(ObjectId{}));
    EXPECT_MODEL_ERROR((void)doc.get(ObjectId{99}), ModelErrc::unknown_object);
}

TEST_F(DocumentTest, TreeKeepsCreationOrder) {
    const ObjectId root1 = doc.create(module_object(module_a, "group", "loads"));
    const ObjectId root2 = doc.create(module_object(module_a, "group", "steps"));
    const ObjectId child1 = doc.create(module_object(module_a, "load", "p2", root1));
    const ObjectId child2 = doc.create(module_object(module_a, "load", "p1", root1));
    const ObjectId grandchild = doc.create(module_object(module_b, "note", "n", child2));

    EXPECT_EQ(std::vector<ObjectId>(doc.roots().begin(), doc.roots().end()),
              (std::vector<ObjectId>{root1, root2}));
    const auto children = doc.get(root1).children();
    EXPECT_EQ(std::vector<ObjectId>(children.begin(), children.end()),
              (std::vector<ObjectId>{child1, child2}));
    EXPECT_EQ(doc.get(child2).parent(), root1);
    EXPECT_EQ(doc.get(grandchild).parent(), child2);
    EXPECT_EQ(ids_of(doc), (std::vector<ObjectId>{root1, root2, child1, child2, grandchild}));
}

TEST_F(DocumentTest, FindByTypeOwnerAndName) {
    const ObjectId group = doc.create(module_object(module_a, "group", "loads"));
    const ObjectId p1 = doc.create(module_object(module_a, "load", "p1", group));
    const ObjectId p2 = doc.create(module_object(module_a, "load", "p2", group));
    const ObjectId note = doc.create(module_object(module_b, "note", "p1", group));

    EXPECT_EQ(doc.find_by_type("load"), (std::vector<ObjectId>{p1, p2}));
    EXPECT_EQ(doc.find_by_type("missing"), std::vector<ObjectId>{});
    EXPECT_EQ(doc.find_by_owner(module_b), std::vector<ObjectId>{note});

    EXPECT_EQ(doc.find_child(group, "load", "p2"), p2);
    EXPECT_EQ(doc.find_child(group, "note", "p1"), note);
    EXPECT_EQ(doc.find_child({}, "group", "loads"), group);
    EXPECT_FALSE(doc.find_child(group, "load", "p3").is_valid());
    EXPECT_FALSE(doc.find_child({}, "load", "p1").is_valid());
}

TEST_F(DocumentTest, OwnersListsTheModulesTheModelNeeds) {
    doc.create(module_object(module_b, "item"));
    doc.create(module_object(module_a, "item"));
    doc.create(module_object(module_a, "item"));
    EXPECT_EQ(doc.owners(), (std::vector<std::string>{module_a, module_b}));
}

TEST_F(DocumentTest, NamesAreUniqueAmongSiblingsOfTheSameType) {
    const ObjectId group = doc.create(module_object(module_a, "group", "g"));
    doc.create(module_object(module_a, "load", "p", group));

    EXPECT_MODEL_ERROR(doc.create(module_object(module_a, "load", "p", group)),
                       ModelErrc::duplicate_name);
    // Other type, other parent and unnamed objects do not clash.
    doc.create(module_object(module_a, "bc", "p", group));
    doc.create(module_object(module_a, "load", "p"));
    doc.create(module_object(module_a, "load", "", group));
    doc.create(module_object(module_a, "load", "", group));
    // Names are case-sensitive.
    doc.create(module_object(module_a, "load", "P", group));
    EXPECT_EQ(doc.find_by_type("load").size(), 5U);
}

TEST_F(DocumentTest, RenameChecksUniquenessAndUpdatesLookup) {
    const ObjectId a = doc.create(module_object(module_a, "load", "a"));
    const ObjectId b = doc.create(module_object(module_a, "load", "b"));

    EXPECT_MODEL_ERROR(doc.rename(b, "a"), ModelErrc::duplicate_name);
    EXPECT_EQ(doc.get(b).name(), "b");

    doc.rename(a, "a");  // Same name: no-op.
    doc.rename(a, "c");
    EXPECT_EQ(doc.get(a).name(), "c");
    EXPECT_EQ(doc.find_child({}, "load", "c"), a);
    EXPECT_FALSE(doc.find_child({}, "load", "a").is_valid());
    doc.rename(b, "a");
    EXPECT_EQ(doc.find_child({}, "load", "a"), b);
    doc.rename(b, "");
    EXPECT_FALSE(doc.find_child({}, "load", "a").is_valid());
}

TEST_F(DocumentTest, CyrillicNamesAndPropertiesAreKept) {
    const std::string name = utf8(u8"Сталь 20 — прокат");
    NewObject init = module_object(module_a, "material", name);
    init.properties = {{utf8(u8"примечание"), utf8(u8"по ГОСТ 1050")}};
    const ObjectId id = doc.create(init);

    EXPECT_EQ(doc.get(id).name(), name);
    EXPECT_EQ(doc.get(id).properties(), init.properties);
    EXPECT_EQ(doc.find_child({}, "material", name), id);
}

TEST_F(DocumentTest, SetPropertiesReplacesTheValue) {
    NewObject init = module_object(module_a, "material");
    init.properties = {{"young", 1.0}, {"poisson", 0.3}};
    const ObjectId id = doc.create(init);

    const Properties replacement = {
        {"young", 2.0}, {"table", {1, 2, 3}}, {"nested", {{"a", true}}}};
    doc.set_properties(id, replacement);
    EXPECT_EQ(doc.get(id).properties(), replacement);
}

TEST_F(DocumentTest, InvalidObjectsAreRejected) {
    EXPECT_MODEL_ERROR(doc.create(module_object(module_a, "")), ModelErrc::invalid_argument);
    EXPECT_MODEL_ERROR(doc.create(module_object("", "item")), ModelErrc::invalid_argument);

    NewObject zero_version = module_object(module_a, "item");
    zero_version.schema_version = 0;
    EXPECT_MODEL_ERROR(doc.create(zero_version), ModelErrc::invalid_argument);

    EXPECT_MODEL_ERROR(doc.create(module_object(module_a, "item", "x", ObjectId{42})),
                       ModelErrc::unknown_object);

    // Types of the core are reserved and carry their own data.
    EXPECT_MODEL_ERROR(doc.create(module_object(std::string(core_owner), "material")),
                       ModelErrc::invalid_argument);
    NewObject with_data = module_object(module_a, "item");
    with_data.data = fem::model::testing::tiny_mesh();
    EXPECT_MODEL_ERROR(doc.create(with_data), ModelErrc::invalid_data);

    EXPECT_EQ(doc.size(), 0U);
}

TEST_F(DocumentTest, PropertiesMustBeFiniteUtf8JsonObjects) {
    const auto create_with = [this](Properties properties) {
        NewObject init = module_object(module_a, "item");
        init.properties = std::move(properties);
        return doc.create(std::move(init));
    };
    const auto set_to = [this](ObjectId id, Properties properties) {
        doc.set_properties(id, std::move(properties));
    };

    EXPECT_MODEL_ERROR(create_with(Properties::array()), ModelErrc::invalid_properties);
    EXPECT_MODEL_ERROR(create_with("text"), ModelErrc::invalid_properties);
    EXPECT_MODEL_ERROR(create_with(nullptr), ModelErrc::invalid_properties);

    const ObjectId id = create_with({{"a", 1}});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_MODEL_ERROR(set_to(id, {{"a", nan}}), ModelErrc::invalid_properties);
    EXPECT_MODEL_ERROR(set_to(id, {{"a", {1.0, -inf}}}), ModelErrc::invalid_properties);
    EXPECT_MODEL_ERROR(set_to(id, {{"a", {{"b", inf}}}}), ModelErrc::invalid_properties);
    EXPECT_MODEL_ERROR(set_to(id, {{"a", Properties::binary({1, 2})}}),
                       ModelErrc::invalid_properties);
    EXPECT_MODEL_ERROR(set_to(id, {{"a", std::string("\xC3\x28")}}), ModelErrc::invalid_properties);
    EXPECT_MODEL_ERROR(set_to(id, {{std::string("\xFF"), 1}}), ModelErrc::invalid_properties);

    EXPECT_EQ(doc.get(id).properties(), (Properties{{"a", 1}}));
}

TEST_F(DocumentTest, NamesMustBeUtf8) {
    EXPECT_MODEL_ERROR(doc.create(module_object(module_a, "item", std::string("\xE2\x82"))),
                       ModelErrc::invalid_name);
    // Overlong encoding of '/' and an encoded UTF-16 surrogate.
    EXPECT_MODEL_ERROR(doc.create(module_object(module_a, "item", std::string("\xC0\xAF"))),
                       ModelErrc::invalid_name);
    EXPECT_MODEL_ERROR(doc.create(module_object(module_a, "item", std::string("\xED\xA0\x80"))),
                       ModelErrc::invalid_name);

    const ObjectId id = doc.create(module_object(module_a, "item", utf8(u8"имя 😀")));
    EXPECT_MODEL_ERROR(doc.rename(id, std::string("\xF8\x88\x80\x80\x80")),
                       ModelErrc::invalid_name);
}

TEST_F(DocumentTest, RemoveTakesTheWholeSubtree) {
    const ObjectId group = doc.create(module_object(module_a, "group", "g"));
    const ObjectId child = doc.create(module_object(module_a, "load", "p", group));
    const ObjectId grandchild = doc.create(module_object(module_a, "note", "n", child));
    const ObjectId sibling = doc.create(module_object(module_a, "load", "q", group));
    const ObjectId other = doc.create(module_object(module_a, "group", "h"));

    const RemoveResult result = doc.remove(child);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.removed, (std::vector<ObjectId>{child, grandchild}));
    EXPECT_EQ(ids_of(doc), (std::vector<ObjectId>{group, sibling, other}));
    const auto children = doc.get(group).children();
    EXPECT_EQ(std::vector<ObjectId>(children.begin(), children.end()),
              std::vector<ObjectId>{sibling});

    // The name is free again.
    doc.create(module_object(module_a, "load", "p", group));

    ASSERT_TRUE(doc.remove(group).ok());
    EXPECT_EQ(std::vector<ObjectId>(doc.roots().begin(), doc.roots().end()),
              std::vector<ObjectId>{other});
    EXPECT_EQ(doc.size(), 1U);
}

TEST_F(DocumentTest, RemovingUnknownObjectThrows) {
    EXPECT_MODEL_ERROR((void)doc.remove(ObjectId{7}), ModelErrc::unknown_object);
    EXPECT_MODEL_ERROR((void)doc.removal_blockers(ObjectId{7}), ModelErrc::unknown_object);
}

TEST_F(DocumentTest, ChangesToUnknownObjectsThrow) {
    EXPECT_MODEL_ERROR(doc.rename(ObjectId{7}, "x"), ModelErrc::unknown_object);
    EXPECT_MODEL_ERROR(doc.set_properties(ObjectId{7}, Properties::object()),
                       ModelErrc::unknown_object);
    EXPECT_MODEL_ERROR(doc.set_data(ObjectId{7}, nullptr), ModelErrc::unknown_object);
}

TEST(DocumentModules, CoreIsAlwaysEnabledAndCannotBeDisabled) {
    Document doc;
    EXPECT_TRUE(doc.is_owner_enabled(core_owner));
    EXPECT_MODEL_ERROR(doc.set_owner_enabled(core_owner, false), ModelErrc::invalid_argument);
    EXPECT_MODEL_ERROR(doc.set_owner_enabled("", true), ModelErrc::invalid_argument);
}

TEST(DocumentModules, ModulesAreDisabledUntilEnabled) {
    Document doc;
    EXPECT_FALSE(doc.is_owner_enabled(module_a));
    doc.set_owner_enabled(module_a, true);
    EXPECT_TRUE(doc.is_owner_enabled(module_a));
    EXPECT_FALSE(doc.is_owner_enabled(module_b));
    doc.set_owner_enabled(module_a, false);
    EXPECT_FALSE(doc.is_owner_enabled(module_a));
}

TEST(ObjectIdTest, ValidityAndFormatting) {
    EXPECT_FALSE(ObjectId{}.is_valid());
    EXPECT_FALSE(static_cast<bool>(ObjectId{}));
    EXPECT_TRUE(ObjectId{5}.is_valid());
    EXPECT_LT(ObjectId{2}, ObjectId{10});
    EXPECT_EQ(to_string(ObjectId{42}), "#42");
    EXPECT_EQ(to_string(ObjectId{}), "#none");
}
