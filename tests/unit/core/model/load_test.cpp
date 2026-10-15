// Loading a document past the commands (task 2.1): `CommandBus::replace_document`.

#include "fem/core/command/error.h"
#include "fem/core/model/contents.h"

#include "model_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace fem::model {
namespace {

using testing::BusEditor;
using testing::module_a;
using testing::module_object;

StoredObject stored(std::uint64_t id, NewObject init) {
    return {ObjectId(id), std::move(init)};
}

/// mesh #2 with node set #5; object #7 of module A under nothing, depending on #5 and on the
/// removed #4; units mm-t-s; next id 9.
DocumentContents sample() {
    DocumentContents contents;
    contents.units = UnitSystem::mm_t_s();
    contents.next_id = 9;
    NewObject note = module_object(module_a, "note", "n", {}, {ObjectId(5), ObjectId(4)});
    note.properties = {{"a", 1.5}};
    // In any order: the document sorts them by id.
    contents.objects.push_back(stored(7, std::move(note)));
    contents.objects.push_back(
        stored(5, node_set_object(ObjectId(2), "fixed",
                                  std::make_shared<const NodeSet>(std::vector<Index>{0, 1}))));
    contents.objects.push_back(stored(2, mesh_object("m", testing::small_mesh())));
    return contents;
}

TEST(ModelLoad, KeepsIdsUnitsAndTheNextId) {
    Document doc;
    BusEditor edit(doc);
    edit.create(module_object(module_a, "old"));
    edit.bus().replace_document(sample());

    EXPECT_EQ(doc.size(), 3U);
    EXPECT_EQ(doc.units(), UnitSystem::mm_t_s());
    EXPECT_EQ(doc.next_id(), 9U);
    EXPECT_EQ(doc.get(ObjectId(5)).parent(), ObjectId(2));
    EXPECT_EQ(doc.find_child(ObjectId(2), "node_set", "fixed"), ObjectId(5));
    EXPECT_EQ(doc.dependents(ObjectId(5)), (std::vector<ObjectId>{ObjectId(7)}));
    EXPECT_EQ(doc.missing_dependencies(ObjectId(7)), (std::vector<ObjectId>{ObjectId(4)}));
    EXPECT_EQ(doc.get(ObjectId(7)).properties().at("a"), 1.5);
    std::vector<ObjectId> roots(doc.roots().begin(), doc.roots().end());
    EXPECT_EQ(roots, (std::vector<ObjectId>{ObjectId(2), ObjectId(7)}));

    // The next object gets the next id, not a reused one.
    doc.set_owner_enabled(module_a, true);
    EXPECT_EQ(edit.create(module_object(module_a, "new")), ObjectId(9));
}

TEST(ModelLoad, ObjectsOfDisabledModulesAreWelcome) {
    Document doc;
    BusEditor edit(doc);
    edit.bus().replace_document(sample());
    EXPECT_TRUE(doc.is_opaque(ObjectId(7)));
    // The node set it depends on cannot be removed now.
    EXPECT_FALSE(edit.remove(ObjectId(5)).ok());
}

TEST(ModelLoad, ClearsTheHistoryAndTellsListeners) {
    Document doc;
    BusEditor edit(doc);
    doc.set_owner_enabled(module_a, true);
    edit.create(module_object(module_a, "x"));  // #1
    edit.create(module_object(module_a, "y"));  // #2
    edit.create(module_object(module_a, "z"));  // #3, undone
    ASSERT_TRUE(edit.bus().can_undo());
    ASSERT_TRUE(edit.bus().undo());
    ASSERT_TRUE(edit.bus().can_redo());

    std::vector<command::ModelChange> events;
    std::vector<std::string> journal;
    const Subscription listening = edit.bus().on_model_changed(
        [&](const command::ModelChange& change) { events.push_back(change); });
    const Subscription journaling =
        edit.bus().on_journal([&](std::string_view entry) { journal.emplace_back(entry); });
    edit.bus().replace_document(sample(), "stresslab.project.open(\"x\")");

    EXPECT_FALSE(edit.bus().can_undo());
    EXPECT_FALSE(edit.bus().can_redo());
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].cause, command::ModelChange::Cause::load);
    EXPECT_EQ(events[0].summary.created, (std::vector<ObjectId>{ObjectId(5), ObjectId(7)}));
    EXPECT_EQ(events[0].summary.updated, (std::vector<ObjectId>{ObjectId(2)}));
    EXPECT_EQ(events[0].summary.removed, (std::vector<ObjectId>{ObjectId(1)}));
    EXPECT_TRUE(events[0].summary.units);
    EXPECT_EQ(journal, (std::vector<std::string>{"stresslab.project.open(\"x\")"}));
    EXPECT_EQ(command::to_string(command::ModelChange::Cause::load), "load");
}

TEST(ModelLoad, RefusedInsideACommand) {
    Document doc;
    command::CommandBus bus(doc);
    bus.register_command({.id = "test.load", .owner = "test"}, [&bus](command::CommandContext&) {
        bus.replace_document({});
        return command::CommandResult{};
    });
    EXPECT_THROW(bus.execute("test.load"), command::CommandError);
}

/// `change` makes the sample invalid: loading throws `code` and the document stays as it was.
void expect_refused(const std::function<void(DocumentContents&)>& change, ModelErrc code) {
    Document doc;
    BusEditor edit(doc);
    doc.set_owner_enabled(module_a, true);
    edit.create(module_object(module_a, "kept"));
    DocumentContents contents = sample();
    change(contents);
    EXPECT_MODEL_ERROR(edit.bus().replace_document(std::move(contents)), code);
    EXPECT_EQ(doc.size(), 1U);
    EXPECT_EQ(doc.get(ObjectId(1)).type(), "kept");
    EXPECT_EQ(doc.next_id(), 2U);
    EXPECT_TRUE(edit.bus().can_undo());
}

TEST(ModelLoad, RefusesInvalidContents) {
    expect_refused([](DocumentContents& c) { c.next_id = 7; }, ModelErrc::invalid_argument);
    expect_refused([](DocumentContents& c) { c.objects[0].id = ObjectId(5); },
                   ModelErrc::invalid_argument);
    expect_refused([](DocumentContents& c) { c.objects[0].id = ObjectId(); },
                   ModelErrc::invalid_argument);
    expect_refused([](DocumentContents& c) { c.objects[1].init.parent = ObjectId(3); },
                   ModelErrc::unknown_object);
    expect_refused([](DocumentContents& c) { c.objects[0].init.type.clear(); },
                   ModelErrc::invalid_argument);
    expect_refused([](DocumentContents& c) { c.objects[0].init.properties = 1; },
                   ModelErrc::invalid_properties);
    expect_refused([](DocumentContents& c) { c.objects[1].init.data = testing::tiny_mesh(); },
                   ModelErrc::invalid_data);
    expect_refused(
        [](DocumentContents& c) { c.objects[0].init.dependencies.push_back(ObjectId(7)); },
        ModelErrc::invalid_dependency);
    expect_refused(
        [](DocumentContents& c) { c.objects[0].init.dependencies.push_back(ObjectId(12)); },
        ModelErrc::invalid_dependency);
    expect_refused(
        [](DocumentContents& c) {
            c.objects[1].init.dependencies.push_back(ObjectId(7));  // 5 -> 7 -> 5
        },
        ModelErrc::dependency_cycle);
    expect_refused(
        [](DocumentContents& c) {
            c.objects.push_back({ObjectId(8), node_set_object(ObjectId(2), "fixed",
                                                              std::make_shared<const NodeSet>(
                                                                  std::vector<Index>{}))});
        },
        ModelErrc::duplicate_name);
}

TEST(ModelLoad, ChildrenAreInIdOrderAfterAnyHistory) {
    // Saving relies on it: children are not stored, they are their parent's in id order.
    Document doc;
    BusEditor edit(doc);
    doc.set_owner_enabled(module_a, true);
    const ObjectId parent = edit.create(module_object(module_a, "p"));
    const ObjectId a = edit.create(module_object(module_a, "c", "a", parent));
    const ObjectId b = edit.create(module_object(module_a, "c", "b", parent));
    (void)edit.remove(a);
    const ObjectId c = edit.create(module_object(module_a, "c", "c", parent));
    ASSERT_TRUE(edit.bus().undo());
    ASSERT_TRUE(edit.bus().undo());  // a is back, before b
    ASSERT_TRUE(edit.bus().redo());
    ASSERT_TRUE(edit.bus().redo());
    std::vector<ObjectId> children(doc.get(parent).children().begin(),
                                   doc.get(parent).children().end());
    EXPECT_EQ(children, (std::vector<ObjectId>{b, c}));
    EXPECT_TRUE(std::is_sorted(children.begin(), children.end()));
}

}  // namespace
}  // namespace fem::model
