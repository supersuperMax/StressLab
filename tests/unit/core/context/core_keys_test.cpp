#include "fem/core/context/core_keys.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/error.h"
#include "fem/core/context/when.h"
#include "fem/core/model/document.h"
#include "fem/core/model/object.h"

#include "../model/model_test_support.h"
#include "context_test_support.h"
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using fem::Subscription;
using fem::context::ContextErrc;
using fem::context::ContextKeys;
using fem::context::ContextValue;
using fem::context::CoreKeys;
using fem::context::WhenExpression;
using fem::model::Document;
using fem::model::mesh_object;
using fem::model::ObjectId;
using fem::model::testing::BusEditor;
using fem::model::testing::module_object;
using fem::model::testing::tiny_mesh;
namespace core_keys = fem::context::core_keys;

class CoreKeysTest : public ::testing::Test {
protected:
    CoreKeysTest() { doc.set_owner_enabled(fem::model::testing::module_a, true); }

    [[nodiscard]] bool key(std::string_view name) const {
        return keys.value(name) == ContextValue(true);
    }

    Document doc;
    BusEditor editor{doc};
    ContextKeys keys;
};

TEST_F(CoreKeysTest, KeysAreDeclaredByTheCore) {
    const CoreKeys core(editor.bus(), keys);
    EXPECT_EQ(keys.keys(),
              (std::vector<std::string>{"history.canRedo", "history.canUndo", "model.hasMesh"}));
    for (const std::string& name : keys.keys()) {
        ASSERT_NE(keys.find(name), nullptr);
        EXPECT_EQ(keys.find(name)->owner, fem::model::core_owner);
        EXPECT_EQ(keys.find(name)->type, ContextValue::Kind::boolean);
        EXPECT_EQ(keys.value(name), ContextValue(false)) << name;
    }
    EXPECT_CONTEXT_ERROR(keys.remove_owner(fem::model::core_owner), ContextErrc::invalid_spec);
}

TEST_F(CoreKeysTest, HasMeshFollowsMeshesOfTheCore) {
    const CoreKeys core(editor.bus(), keys);
    // An object of a module is not a mesh of the core, whatever its type is called.
    editor.create(module_object(fem::model::testing::module_a, "mesh", "fake"));
    EXPECT_FALSE(key(core_keys::has_mesh));

    const ObjectId first = editor.create(mesh_object("first", tiny_mesh()));
    EXPECT_TRUE(key(core_keys::has_mesh));
    const ObjectId second = editor.create(mesh_object("second", tiny_mesh()));
    editor.remove(first);
    EXPECT_TRUE(key(core_keys::has_mesh));
    editor.remove(second);
    EXPECT_FALSE(key(core_keys::has_mesh));

    ASSERT_TRUE(editor.bus().undo());
    EXPECT_TRUE(key(core_keys::has_mesh));
    ASSERT_TRUE(editor.bus().redo());
    EXPECT_FALSE(key(core_keys::has_mesh));
}

TEST_F(CoreKeysTest, HistoryKeysFollowTheBus) {
    const CoreKeys core(editor.bus(), keys);
    editor.create(module_object(fem::model::testing::module_a, "material", "steel"));
    EXPECT_TRUE(key(core_keys::can_undo));
    EXPECT_FALSE(key(core_keys::can_redo));

    ASSERT_TRUE(editor.bus().undo());
    EXPECT_FALSE(key(core_keys::can_undo));
    EXPECT_TRUE(key(core_keys::can_redo));

    ASSERT_TRUE(editor.bus().redo());
    EXPECT_TRUE(key(core_keys::can_undo));
    EXPECT_FALSE(key(core_keys::can_redo));

    ASSERT_TRUE(editor.bus().undo());
    editor.create(module_object(fem::model::testing::module_a, "material", "aluminium"));
    EXPECT_TRUE(key(core_keys::can_undo));
    EXPECT_FALSE(key(core_keys::can_redo));
}

TEST_F(CoreKeysTest, KeysChangeWhenTheTransactionIsCommitted) {
    const CoreKeys core(editor.bus(), keys);
    {
        fem::command::Transaction rolled_back = editor.bus().begin_transaction("discarded");
        editor.create(mesh_object("mesh", tiny_mesh()));
        EXPECT_FALSE(key(core_keys::has_mesh));
    }
    EXPECT_FALSE(key(core_keys::has_mesh));
    EXPECT_FALSE(key(core_keys::can_undo));

    fem::command::Transaction transaction = editor.bus().begin_transaction("import");
    editor.create(mesh_object("mesh", tiny_mesh()));
    EXPECT_FALSE(key(core_keys::has_mesh));
    EXPECT_FALSE(key(core_keys::can_undo));
    transaction.commit();
    EXPECT_TRUE(key(core_keys::has_mesh));
    EXPECT_TRUE(key(core_keys::can_undo));
}

TEST_F(CoreKeysTest, WatchedExpressionsSeeTheModel) {
    const CoreKeys core(editor.bus(), keys);
    std::vector<bool> calls;
    const Subscription watch =
        keys.watch(WhenExpression::parse("model.hasMesh && !history.canRedo"),
                   [&](bool value) { calls.push_back(value); });

    editor.create(mesh_object("mesh", tiny_mesh()));
    ASSERT_TRUE(editor.bus().undo());
    ASSERT_TRUE(editor.bus().redo());
    EXPECT_EQ(calls, (std::vector<bool>{true, false, true}));
}

TEST_F(CoreKeysTest, NextDocumentTakesTheKeysOver) {
    const CoreKeys core(editor.bus(), keys);
    editor.create(mesh_object("mesh", tiny_mesh()));
    EXPECT_TRUE(key(core_keys::has_mesh));

    Document other_doc;
    other_doc.set_owner_enabled(fem::model::testing::module_a, true);
    BusEditor other(other_doc);
    other.create(mesh_object("mesh", tiny_mesh()));
    other.create(module_object(fem::model::testing::module_a, "material", "steel"));
    ASSERT_TRUE(other.bus().undo());
    {
        const CoreKeys next(other.bus(), keys);
        EXPECT_TRUE(key(core_keys::has_mesh));
        EXPECT_TRUE(key(core_keys::can_undo));
        EXPECT_TRUE(key(core_keys::can_redo));
        ASSERT_TRUE(other.bus().undo());
        EXPECT_FALSE(key(core_keys::has_mesh));
    }
    EXPECT_EQ(keys.keys().size(), 3U);
}

TEST_F(CoreKeysTest, KeysReflectTheDocumentWhenCreated) {
    editor.create(mesh_object("mesh", tiny_mesh()));
    const CoreKeys core(editor.bus(), keys);
    EXPECT_TRUE(key(core_keys::has_mesh));
    EXPECT_TRUE(key(core_keys::can_undo));
}

}  // namespace
