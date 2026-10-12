// Undo and redo (docs/interface-design.md §2, principle 6): every step is reverted and repeated
// exactly, including ids, the order of children, name lookup and the dependency index.

#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
#include "fem/core/command/model_commands.h"
#include "fem/core/model/error.h"
#include "fem/core/model/mesh.h"

#include "../model/model_test_support.h"
#include "command_test_support.h"
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace fem::command;
using fem::command::testing::dump;
using fem::command::testing::JournalRecorder;
using fem::command::testing::test_owner;
using fem::command::testing::utf8;
using fem::model::Document;
using fem::model::Mesh;
using fem::model::ModelErrc;
using fem::model::ModelError;
using fem::model::ObjectId;
using fem::model::UnitSystem;
namespace mc = fem::command::model_commands;

namespace {

constexpr const char* other_owner = "test.other";

class UndoTest : public ::testing::Test {
protected:
    void SetUp() override {
        doc.set_owner_enabled(test_owner, true);
        doc.set_owner_enabled(other_owner, true);
    }

    ObjectId create(const std::string& name, ObjectId parent = {},
                    const std::vector<ObjectId>& dependencies = {},
                    const std::string& owner = test_owner) {
        Value::Dict args{{"type", "item"}, {"owner", owner}, {"name", name}};
        if (parent.is_valid()) {
            args["parent"] = parent;
        }
        if (!dependencies.empty()) {
            Value::List list;
            for (const ObjectId id : dependencies) {
                list.emplace_back(id);
            }
            args["dependencies"] = std::move(list);
        }
        return ObjectId(
            static_cast<std::uint64_t>(bus.execute(mc::create, args).value.as_integer()));
    }

    [[nodiscard]] bool exists(const std::string& name) const {
        return doc.find_child({}, "item", name).is_valid();
    }

    Document doc;
    CommandBus bus{doc};
    JournalRecorder journal{bus};
};

}  // namespace

TEST_F(UndoTest, NothingToUndoOrRedoAtFirst) {
    EXPECT_FALSE(bus.can_undo());
    EXPECT_FALSE(bus.can_redo());
    EXPECT_EQ(bus.undo_label(), "");
    EXPECT_FALSE(bus.undo());
    EXPECT_FALSE(bus.redo());
    EXPECT_TRUE(journal.entries().empty());
}

TEST_F(UndoTest, CreateIsUndoneAndRedoneWithTheSameId) {
    const std::string empty = dump(doc);
    const ObjectId id = create("a");
    const std::string created = dump(doc);
    EXPECT_EQ(bus.undo_label(), mc::create);

    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(dump(doc), empty);
    EXPECT_FALSE(bus.can_undo());
    EXPECT_EQ(bus.redo_label(), mc::create);

    ASSERT_TRUE(bus.redo());
    EXPECT_EQ(dump(doc), created);
    EXPECT_TRUE(doc.contains(id));
    EXPECT_FALSE(bus.can_redo());

    const std::string expected_journal =
        "stresslab.cmd(\"model.create\", type=\"item\", owner=\"test.module\", name=\"a\")\n"
        "stresslab.undo()\n"
        "stresslab.redo()\n";
    EXPECT_EQ(journal.text(), expected_journal);
}

TEST_F(UndoTest, UndoneIdsAreNotReused) {
    EXPECT_EQ(create("a"), ObjectId{1});
    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(create("b"), ObjectId{2});
}

TEST_F(UndoTest, NewStepClearsRedo) {
    create("a");
    create("b");
    ASSERT_TRUE(bus.undo());
    EXPECT_TRUE(bus.can_redo());
    create("c");
    EXPECT_FALSE(bus.can_redo());
    EXPECT_FALSE(bus.redo());
}

TEST_F(UndoTest, CommandThatChangesNothingIsNoStep) {
    const ObjectId a = create("a");
    bus.execute(mc::rename, {{"object", a}, {"name", "a"}});
    bus.execute(mc::set_units,
                {{"length", "m"}, {"mass", "kg"}, {"time", "s"}, {"temperature", "K"}});
    EXPECT_EQ(bus.undo_label(), mc::create);
    EXPECT_EQ(journal.entries().size(), 3U);  // Journaled anyway: they were called.
}

TEST_F(UndoTest, EveryKindOfChangeIsUndoneAndRedoneExactly) {
    const ObjectId a = create("a");
    const ObjectId b = create("b", a);
    const ObjectId c = create("c");

    const std::vector<Value::Dict> steps = {
        {{"command", mc::rename}, {"object", b}, {"name", utf8(u8"бэ")}},
        {{"command", mc::rename}, {"object", b}, {"name", ""}},
        {{"command", mc::set_properties},
         {"object", a},
         {"properties", Value::Dict{{"E", 2.1e5}, {"list", Value::List{1, "x"}}}}},
        {{"command", mc::add_dependency}, {"object", c}, {"target", b}},
        {{"command", mc::add_dependency}, {"object", c}, {"target", a}},
        {{"command", mc::remove_dependency}, {"object", c}, {"target", b}},
        {{"command", mc::set_units},
         {"length", "mm"},
         {"mass", "t"},
         {"time", "s"},
         {"temperature", "degC"}},
    };
    for (Value::Dict step : steps) {
        const std::string command = step.at("command").as_string();
        step.erase("command");
        const std::string before = dump(doc);
        bus.execute(command, step);
        const std::string after = dump(doc);
        ASSERT_NE(after, before) << command;

        ASSERT_TRUE(bus.undo());
        EXPECT_EQ(dump(doc), before) << command;
        ASSERT_TRUE(bus.redo());
        EXPECT_EQ(dump(doc), after) << command;
    }
}

TEST_F(UndoTest, RemovedSubtreeComesBackInPlace) {
    // group -> [first, middle -> [inner -> [leaf], named], last]; outside depends on inner, and
    // leaf depends on outside: undo must restore children order, names and both index directions.
    const ObjectId group = create("group");
    const ObjectId first = create("first", group);
    const ObjectId middle = create("middle", group);
    const ObjectId last = create("last", group);
    const ObjectId inner = create("inner", middle);
    const ObjectId outside = create("outside", {}, {inner});
    const ObjectId leaf = create(utf8(u8"лист"), inner, {outside});
    const ObjectId named = create("named", middle);
    bus.execute(mc::add_dependency, {{"object", first}, {"target", middle}});
    const std::string before = dump(doc);

    const CommandResult removed = bus.execute(mc::remove, {{"object", middle}});
    EXPECT_EQ(removed.value, Value(Value::List{middle, inner, leaf, named}));
    EXPECT_EQ(doc.missing_dependencies(outside), std::vector<ObjectId>{inner});
    EXPECT_EQ(doc.missing_dependencies(first), std::vector<ObjectId>{middle});
    EXPECT_TRUE(doc.dependents(outside).empty());
    const std::string after = dump(doc);

    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(dump(doc), before);
    const auto children = doc.get(group).children();
    EXPECT_EQ(std::vector<ObjectId>(children.begin(), children.end()),
              (std::vector<ObjectId>{first, middle, last}));
    EXPECT_EQ(doc.find_child(inner, "item", utf8(u8"лист")), leaf);
    EXPECT_EQ(doc.dependents(outside), std::vector<ObjectId>{leaf});

    ASSERT_TRUE(bus.redo());
    EXPECT_EQ(dump(doc), after);
    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(dump(doc), before);
}

TEST_F(UndoTest, BulkDataIsRestoredWithoutCopies) {
    const std::shared_ptr<const Mesh> coarse = fem::model::testing::small_mesh();
    const std::shared_ptr<const Mesh> fine = fem::model::testing::tiny_mesh();
    bus.register_command(
        {.id = "test.mesh",
         .owner = test_owner,
         .args = {{.name = "fine", .type = ArgType::boolean}}},
        [&](CommandContext& context) {
            const auto& data = context.args().boolean("fine") ? fine : coarse;
            const ObjectId mesh = context.document().find_child({}, "mesh", "part");
            if (mesh.is_valid()) {
                context.editor().set_data(mesh, data);
                return Value(mesh);
            }
            return Value(context.editor().create(fem::model::mesh_object("part", data)));
        });
    const ObjectId mesh = ObjectId(
        static_cast<std::uint64_t>(bus.execute("test.mesh", {{"fine", false}}).value.as_integer()));
    bus.execute("test.mesh", {{"fine", true}});
    EXPECT_EQ(doc.get(mesh).data(), fine);

    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(doc.get(mesh).data(), coarse);
    ASSERT_TRUE(bus.undo());
    EXPECT_FALSE(doc.contains(mesh));
    ASSERT_TRUE(bus.redo());
    EXPECT_EQ(doc.get(mesh).data(), coarse);
    ASSERT_TRUE(bus.redo());
    EXPECT_EQ(doc.get(mesh).data(), fine);
}

TEST_F(UndoTest, WholeHistoryWalksBackAndForth) {
    std::vector<std::string> states{dump(doc)};
    const ObjectId a = create("a");
    states.push_back(dump(doc));
    const ObjectId b = create("b", a);
    states.push_back(dump(doc));
    bus.execute(mc::set_properties, {{"object", b}, {"properties", Value::Dict{{"x", 1}}}});
    states.push_back(dump(doc));
    const ObjectId c = create("c", {}, {b});
    states.push_back(dump(doc));
    bus.execute(mc::remove, {{"object", a}});
    states.push_back(dump(doc));
    bus.execute(mc::rename, {{"object", c}, {"name", "c2"}});
    states.push_back(dump(doc));

    for (std::size_t i = states.size() - 1; i > 0; --i) {
        ASSERT_TRUE(bus.undo());
        EXPECT_EQ(dump(doc), states[i - 1]) << "after undo to state " << i - 1;
    }
    EXPECT_FALSE(bus.can_undo());
    for (std::size_t i = 1; i < states.size(); ++i) {
        ASSERT_TRUE(bus.redo());
        EXPECT_EQ(dump(doc), states[i]) << "after redo to state " << i;
    }
    EXPECT_FALSE(bus.can_redo());
}

TEST_F(UndoTest, EventsDescribeTheNetEffectOfEachStep) {
    std::vector<ModelChange> events;
    const fem::Subscription subscription =
        bus.on_model_changed([&](const ModelChange& change) { events.push_back(change); });
    using Cause = ModelChange::Cause;
    using Ids = std::vector<ObjectId>;

    const ObjectId a = create("a");
    const ObjectId b = create("b", a);
    bus.execute(mc::rename, {{"object", b}, {"name", "bb"}});
    bus.execute(mc::rename, {{"object", b}, {"name", "bb"}});  // No change, no event.
    bus.execute(mc::remove, {{"object", a}});
    bus.undo();
    bus.redo();
    bus.execute(mc::set_units,
                {{"length", "mm"}, {"mass", "t"}, {"time", "s"}, {"temperature", "K"}});

    ASSERT_EQ(events.size(), 7U);
    EXPECT_EQ(events[0].cause, Cause::execute);
    EXPECT_EQ(events[0].label, mc::create);
    EXPECT_EQ(events[0].summary.created, Ids{a});
    EXPECT_EQ(events[1].summary.created, Ids{b});
    EXPECT_EQ(events[2].label, mc::rename);
    EXPECT_EQ(events[2].summary.updated, Ids{b});
    EXPECT_TRUE(events[2].summary.created.empty());
    EXPECT_EQ(events[3].label, mc::remove);
    EXPECT_EQ(events[3].summary.removed, (Ids{a, b}));
    EXPECT_EQ(events[4].cause, Cause::undo);
    EXPECT_EQ(events[4].label, mc::remove);
    EXPECT_EQ(events[4].summary.created, (Ids{a, b}));
    EXPECT_EQ(events[5].cause, Cause::redo);
    EXPECT_EQ(events[5].summary.removed, (Ids{a, b}));
    EXPECT_TRUE(events[6].summary.units);
    EXPECT_TRUE(events[6].summary.created.empty());
}

TEST_F(UndoTest, ObjectCreatedAndChangedInOneStepIsReportedAsCreated) {
    std::vector<ModelChange> events;
    const fem::Subscription subscription =
        bus.on_model_changed([&](const ModelChange& change) { events.push_back(change); });
    bus.register_command({.id = "test.build", .owner = test_owner}, [](CommandContext& context) {
        auto& editor = context.editor();
        const ObjectId kept = editor.create({.type = "item", .owner = test_owner, .name = "kept"});
        editor.rename(kept, "renamed");
        const ObjectId temporary = editor.create({.type = "item", .owner = test_owner});
        (void)editor.remove(temporary);
        return CommandResult{};
    });
    bus.execute("test.build");
    bus.undo();

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].summary.created, std::vector<ObjectId>{ObjectId{1}});
    EXPECT_TRUE(events[0].summary.removed.empty());
    EXPECT_TRUE(events[0].summary.updated.empty());
    EXPECT_EQ(events[1].summary.removed, std::vector<ObjectId>{ObjectId{1}});
    EXPECT_TRUE(events[1].summary.created.empty());
    EXPECT_TRUE(events[1].summary.updated.empty());
}

TEST_F(UndoTest, UndoThatWouldChangeAnObjectOfADisabledModuleIsRefused) {
    const ObjectId object = create("a", {}, {}, other_owner);
    bus.execute(mc::rename, {{"object", object}, {"name", "b"}});
    doc.set_owner_enabled(other_owner, false);
    const std::string before = dump(doc);

    try {
        bus.undo();
        ADD_FAILURE() << "undo changed an opaque object";
    } catch (const ModelError& error) {
        EXPECT_EQ(error.code(), ModelErrc::opaque_object);
        EXPECT_EQ(error.object(), object);
    }
    EXPECT_EQ(dump(doc), before);
    EXPECT_TRUE(bus.can_undo());
    EXPECT_FALSE(bus.can_redo());
    EXPECT_EQ(journal.entries().size(), 2U);

    doc.set_owner_enabled(other_owner, true);
    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(doc.get(object).name(), "a");
}

TEST_F(UndoTest, UndoThatWouldRestoreAnObjectOfADisabledModuleIsRefused) {
    const ObjectId object = create("a", {}, {}, other_owner);
    bus.execute(mc::remove, {{"object", object}});
    doc.set_owner_enabled(other_owner, false);
    EXPECT_THROW(bus.undo(), ModelError);
    EXPECT_FALSE(doc.contains(object));
}

TEST_F(UndoTest, RedoThatWouldBreakAnObjectOfADisabledModuleIsRefused) {
    // The active object `user` of another module may lose its target; once the module is
    // disabled, `user` is opaque and the same removal is not allowed any more.
    const ObjectId target = create("target");
    const ObjectId user = create("user", {}, {target}, other_owner);
    bus.execute(mc::remove, {{"object", target}});
    ASSERT_TRUE(bus.undo());
    doc.set_owner_enabled(other_owner, false);
    const std::string before = dump(doc);

    try {
        bus.redo();
        ADD_FAILURE() << "redo broke an opaque object";
    } catch (const ModelError& error) {
        EXPECT_EQ(error.code(), ModelErrc::opaque_object);
        EXPECT_EQ(error.object(), user);
    }
    EXPECT_EQ(dump(doc), before);
    EXPECT_TRUE(bus.can_redo());
}

TEST_F(UndoTest, UndoWorksAfterTheCommandWasUnregistered) {
    bus.register_command({.id = "test.add", .owner = test_owner}, [](CommandContext& context) {
        return Value(context.editor().create({.type = "item", .owner = test_owner}));
    });
    bus.execute("test.add");
    bus.unregister_owner(test_owner);
    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(doc.size(), 0U);
    ASSERT_TRUE(bus.redo());
    EXPECT_EQ(doc.size(), 1U);
}

TEST_F(UndoTest, TheUndoLimitForgetsTheOldestSteps) {
    EXPECT_EQ(bus.undo_limit(), 0U) << "no limit by default";
    bus.set_undo_limit(3);
    for (const char* name : {"a", "b", "c", "d", "e"}) {
        create(name);
    }
    EXPECT_EQ(bus.undo_count(), 3U);
    EXPECT_TRUE(bus.undo());
    EXPECT_TRUE(bus.undo());
    EXPECT_TRUE(bus.undo());
    EXPECT_FALSE(bus.undo()) << "the steps creating a and b are forgotten";
    EXPECT_TRUE(exists("a"));
    EXPECT_TRUE(exists("b"));
    EXPECT_FALSE(exists("c"));
    // Steps to redo are not limited, and redoing them refills the stack up to the limit.
    EXPECT_TRUE(bus.redo());
    EXPECT_TRUE(bus.redo());
    EXPECT_TRUE(bus.redo());
    EXPECT_EQ(bus.undo_count(), 3U);
}

TEST_F(UndoTest, LoweringTheUndoLimitForgetsStepsAtOnce) {
    for (const char* name : {"a", "b", "c", "d"}) {
        create(name);
    }
    bus.set_undo_limit(1);
    EXPECT_EQ(bus.undo_count(), 1U);
    EXPECT_EQ(bus.undo_label(), mc::create);
    EXPECT_TRUE(bus.undo());
    EXPECT_FALSE(exists("d"));
    EXPECT_FALSE(bus.undo());
    bus.set_undo_limit(0);
    create("e");
    create("f");
    EXPECT_EQ(bus.undo_count(), 2U);
}
