// model.set_property: one property changed by a form field, undone and journaled as one line.

#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
#include "fem/core/command/journal.h"
#include "fem/core/command/model_commands.h"
#include "fem/core/model/error.h"

#include "command_test_support.h"
#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace fem::command;
using fem::command::testing::dump;
using fem::command::testing::JournalRecorder;
using fem::command::testing::test_owner;
using fem::command::testing::utf8;
using fem::model::Document;
using fem::model::ObjectId;
using fem::model::Properties;
namespace mc = fem::command::model_commands;

namespace {

struct Session {
    Session() { doc.set_owner_enabled(test_owner, true); }

    Document doc;
    CommandBus bus{doc};
    JournalRecorder journal{bus};
};

class SetPropertyTest : public ::testing::Test {
protected:
    ObjectId create(Properties properties) {
        Value::Dict values;
        for (const auto& [key, value] : properties.items()) {
            values.emplace(key, Value(value.get<double>()));
        }
        return ObjectId(
            static_cast<std::uint64_t>(s.bus
                                           .execute(mc::create, {{"type", "material"},
                                                                 {"owner", test_owner},
                                                                 {"properties", std::move(values)}})
                                           .value.as_integer()));
    }

    void set(ObjectId id, std::string key, Value value) {
        s.bus.execute(mc::set_property,
                      {{"object", id}, {"key", std::move(key)}, {"value", std::move(value)}});
    }

    Session s;
};

}  // namespace

TEST_F(SetPropertyTest, ChangesOnePropertyAndKeepsTheOthers) {
    const ObjectId id = create({{"E", 2.1e11}, {"nu", 0.3}});
    set(id, "E", 2.0e11);
    EXPECT_EQ(s.doc.get(id).properties(), (Properties{{"E", 2.0e11}, {"nu", 0.3}}));
    set(id, "rho", 7850);
    EXPECT_EQ(s.doc.get(id).properties(), (Properties{{"E", 2.0e11}, {"nu", 0.3}, {"rho", 7850}}));
    set(id, "name", utf8(u8"Сталь"));
    EXPECT_EQ(s.doc.get(id).properties().at("name"), utf8(u8"Сталь"));
}

TEST_F(SetPropertyTest, NoneRemovesTheProperty) {
    const ObjectId id = create({{"E", 2.1e11}, {"nu", 0.3}});
    set(id, "nu", Value());
    EXPECT_EQ(s.doc.get(id).properties(), (Properties{{"E", 2.1e11}}));
    set(id, "missing", Value());
    set(id, "a/b", Value());
    EXPECT_EQ(s.doc.get(id).properties(), (Properties{{"E", 2.1e11}}));
}

TEST_F(SetPropertyTest, KeyIsAPathIntoNestedObjects) {
    const ObjectId id = create({});
    set(id, "solver/tolerance", 1e-6);
    set(id, "solver/iterations", 50);
    EXPECT_EQ(s.doc.get(id).properties(),
              (Properties{{"solver", {{"iterations", 50}, {"tolerance", 1e-6}}}}));
    set(id, "solver/tolerance", Value());
    EXPECT_EQ(s.doc.get(id).properties(), (Properties{{"solver", {{"iterations", 50}}}}));
    // '~1' is '/' and '~0' is '~' in a name, as in a JSON Pointer.
    set(id, "a~1b~0c", true);
    EXPECT_EQ(s.doc.get(id).properties().at("a/b~c"), true);
}

TEST_F(SetPropertyTest, RefusesBadKeysAndChangesNothing) {
    const ObjectId id = create({{"E", 1.0}});
    const std::string before = dump(s.doc);
    for (const std::string key : {"", "/E", "E/", "a//b", "a~2", "E~"}) {
        SCOPED_TRACE(key);
        EXPECT_COMMAND_ERROR(set(id, key, 1), CommandErrc::invalid_arguments);
    }
    // A name on the path that holds something else than an object.
    EXPECT_COMMAND_ERROR(set(id, "E/x", 1), CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(set(id, "p", Value(std::filesystem::path("a"))),
                         CommandErrc::invalid_arguments);
    EXPECT_EQ(dump(s.doc), before);
    EXPECT_EQ(s.journal.entries().size(), 1U);  // Only the creation.
}

TEST_F(SetPropertyTest, IsOneUndoStepAndOneJournalLine) {
    const ObjectId id = create({{"E", 2.1e11}, {"nu", 0.3}});
    set(id, "E", 2.0e11);
    set(id, "nu", Value());
    EXPECT_EQ(s.journal.entries().back(),
              "stresslab.cmd(\"model.set_property\", object=1, key=\"nu\", value=None)");
    EXPECT_EQ(s.journal.entries()[1],
              "stresslab.cmd(\"model.set_property\", object=1, key=\"E\", value=200000000000.0)");
    EXPECT_EQ(s.bus.undo_label(), mc::set_property);

    ASSERT_TRUE(s.bus.undo());
    ASSERT_TRUE(s.bus.undo());
    EXPECT_EQ(s.doc.get(id).properties(), (Properties{{"E", 2.1e11}, {"nu", 0.3}}));
    ASSERT_TRUE(s.bus.redo());
    EXPECT_EQ(s.doc.get(id).properties(), (Properties{{"E", 2.0e11}, {"nu", 0.3}}));
}

TEST_F(SetPropertyTest, JournalReplaysToTheSameModel) {
    const ObjectId id = create({{"E", 2.1e11}});
    set(id, "E", 2.0e11);
    set(id, "solver/method", "direct");
    set(id, "flags", Value::List{true, 1, "x"});

    Session replayed;
    replay_journal(s.journal.file(), replayed.bus);
    EXPECT_EQ(dump(replayed.doc), dump(s.doc));
    EXPECT_EQ(replayed.journal.file(), s.journal.file());
}
