// Command registry, argument schema and running of commands (docs/architecture.md §4.2, §4.6).

#include "fem/core/command/command_bus.h"

#include "fem/core/command/error.h"
#include "fem/core/command/model_commands.h"
#include "fem/core/model/error.h"
#include "fem/core/task/task.h"
#include "fem/core/task/task_pool.h"

#include "command_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fem::command;
using fem::command::testing::dump;
using fem::command::testing::JournalRecorder;
using fem::command::testing::test_owner;
using fem::command::testing::utf8;
using fem::model::Document;
using fem::model::ModelErrc;
using fem::model::ModelError;
using fem::model::ObjectId;
namespace fs = std::filesystem;
namespace mc = fem::command::model_commands;

namespace {

class CommandBusTest : public ::testing::Test {
protected:
    void SetUp() override { doc.set_owner_enabled(test_owner, true); }

    /// Registers "test.echo", which returns its arguments as it sees them.
    void register_echo() {
        bus.register_command(
            {.id = "test.echo",
             .owner = test_owner,
             .args = {{.name = "text", .type = ArgType::string},
                      {.name = "count",
                       .type = ArgType::integer,
                       .required = false,
                       .default_value = Value(1)},
                      {.name = "scale", .type = ArgType::number, .required = false},
                      {.name = "file", .type = ArgType::path, .required = false},
                      {.name = "extra", .type = ArgType::any, .required = false}}},
            [](CommandContext& context) { return Value(context.args().values()); });
    }

    ObjectId create(const std::string& name) {
        return ObjectId(static_cast<std::uint64_t>(
            bus.execute(mc::create, {{"type", "item"}, {"owner", test_owner}, {"name", name}})
                .value.as_integer()));
    }

    Document doc;
    CommandBus bus{doc};
    JournalRecorder journal{bus};
};

}  // namespace

TEST_F(CommandBusTest, CommandsOfTheCoreAreRegistered) {
    const std::vector<std::string> ids = bus.command_ids();
    for (const std::string_view id : {mc::create, mc::rename, mc::set_properties, mc::remove,
                                      mc::add_dependency, mc::remove_dependency, mc::set_units}) {
        EXPECT_NE(std::find(ids.begin(), ids.end(), id), ids.end()) << id;
        ASSERT_NE(bus.find_command(id), nullptr);
        EXPECT_EQ(bus.find_command(id)->owner, "fem.core");
    }
    EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
}

TEST_F(CommandBusTest, RegisteredCommandRunsWithCheckedArguments) {
    register_echo();
    EXPECT_EQ(bus.find_command("test.echo")->owner, test_owner);

    const CommandResult result = bus.execute("test.echo", {{"text", utf8(u8"привет")}});
    EXPECT_EQ(result.value, Value(Value::Dict{{"text", utf8(u8"привет")}, {"count", 1}}));
    EXPECT_EQ(result.task, nullptr);
    EXPECT_EQ(journal.text(), utf8(u8"stresslab.cmd(\"test.echo\", text=\"привет\")\n"));
}

TEST_F(CommandBusTest, ArgumentsAreNormalizedAndJournaledInSchemaOrder) {
    register_echo();
    const CommandResult result = bus.execute(
        "test.echo",
        {{"scale", 2}, {"file", utf8(u8"модели/a b.dat")}, {"text", "x"}, {"count", 3}});
    const Value::Dict& seen = result.value.as_dict();
    EXPECT_EQ(seen.at("scale"), Value(2.0));
    EXPECT_EQ(seen.at("file"), Value(fs::path(u8"модели/a b.dat")));
    EXPECT_EQ(journal.entries(),
              std::vector<std::string>{utf8(u8"stresslab.cmd(\"test.echo\", text=\"x\", count=3, "
                                            u8"scale=2.0, file=Path(\"модели/a b.dat\"))")});
    EXPECT_EQ(bus.format_call("test.echo", {{"text", "y"}, {"extra", Value::List{1, "z"}}}),
              "stresslab.cmd(\"test.echo\", text=\"y\", extra=[1, \"z\"])");
}

TEST_F(CommandBusTest, NoneForAnOptionalArgumentMeansOmitted) {
    register_echo();
    const CommandResult result =
        bus.execute("test.echo", {{"text", "x"}, {"count", nullptr}, {"scale", nullptr}});
    EXPECT_EQ(result.value, Value(Value::Dict{{"text", "x"}, {"count", 1}}));
    EXPECT_EQ(journal.entries(),
              std::vector<std::string>{"stresslab.cmd(\"test.echo\", text=\"x\")"});
}

TEST_F(CommandBusTest, ArgumentsThatDoNotMatchTheSchemaAreRefused) {
    register_echo();
    EXPECT_COMMAND_ERROR(bus.execute("test.echo"), CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(bus.execute("test.echo", {{"text", nullptr}}),
                         CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(bus.execute("test.echo", {{"text", "x"}, {"unknown", 1}}),
                         CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(bus.execute("test.echo", {{"text", 5}}), CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(bus.execute("test.echo", {{"text", "x"}, {"count", 1.0}}),
                         CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(bus.execute("test.echo", {{"text", "x"}, {"scale", "big"}}),
                         CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(bus.execute("test.echo", {{"text", "x"}, {"file", true}}),
                         CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(bus.execute(mc::rename, {{"object", 0}, {"name", "x"}}),
                         CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(bus.execute(mc::rename, {{"object", "1"}, {"name", "x"}}),
                         CommandErrc::invalid_arguments);
    EXPECT_TRUE(journal.entries().empty());
}

TEST_F(CommandBusTest, ValuesTheJournalCannotHoldAreRefusedBeforeRunning) {
    int runs = 0;
    bus.register_command(
        {.id = "test.count", .owner = test_owner, .args = {{.name = "value", .required = false}}},
        [&](CommandContext&) {
            ++runs;
            return CommandResult{};
        });
    EXPECT_COMMAND_ERROR(
        bus.execute("test.count", {{"value", std::numeric_limits<double>::infinity()}}),
        CommandErrc::invalid_value);
    EXPECT_COMMAND_ERROR(bus.execute("test.count", {{"value", std::string("\xFF")}}),
                         CommandErrc::invalid_value);
    EXPECT_EQ(runs, 0);
    bus.execute("test.count");
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(journal.text(), "stresslab.cmd(\"test.count\")\n");
}

TEST_F(CommandBusTest, UnknownCommandIsRefused) {
    EXPECT_COMMAND_ERROR(bus.execute("test.missing"), CommandErrc::unknown_command);
    EXPECT_EQ(bus.find_command("test.missing"), nullptr);
}

TEST_F(CommandBusTest, InvalidSpecsAreRefused) {
    const auto handler = [](CommandContext&) { return CommandResult{}; };
    const auto refuse = [&](CommandSpec spec) {
        EXPECT_COMMAND_ERROR(bus.register_command(std::move(spec), handler),
                             CommandErrc::invalid_spec);
    };
    for (const char* id : {"", "nodot", ".a", "a.", "a..b", "1a.b", "a.b c", "a.-b", "a.b/c"}) {
        refuse({.id = id, .owner = test_owner});
    }
    refuse({.id = utf8(u8"test.команда"), .owner = test_owner});
    refuse({.id = "test.x", .owner = ""});
    for (const char* name : {"", "Name", "1x", "a-b", "with", "class", "lambda", "x y"}) {
        refuse({.id = "test.x", .owner = test_owner, .args = {{.name = name}}});
    }
    refuse({.id = "test.x", .owner = test_owner, .args = {{.name = "a"}, {.name = "a"}}});
    refuse({.id = "test.x",
            .owner = test_owner,
            .args = {{.name = "a", .type = ArgType::integer, .default_value = Value(1)}}});
    refuse({.id = "test.x",
            .owner = test_owner,
            .args = {{.name = "a",
                      .type = ArgType::integer,
                      .required = false,
                      .default_value = Value("1")}}});
    EXPECT_COMMAND_ERROR(bus.register_command({.id = "test.x", .owner = test_owner}, nullptr),
                         CommandErrc::invalid_spec);
    EXPECT_EQ(bus.find_command("test.x"), nullptr);

    // Valid: ids with several segments, digits, '_' and '-'; soft keywords as names.
    bus.register_command({.id = "fem.format.ansys-cdb.export_2",
                          .owner = test_owner,
                          .args = {{.name = "type"}, {.name = "match"}, {.name = "_x1"}}},
                         handler);
}

TEST_F(CommandBusTest, DuplicateIdIsRefused) {
    register_echo();
    EXPECT_COMMAND_ERROR(register_echo(), CommandErrc::duplicate_command);
    EXPECT_COMMAND_ERROR(bus.register_command({.id = std::string(mc::create), .owner = test_owner},
                                              [](CommandContext&) { return CommandResult{}; }),
                         CommandErrc::duplicate_command);
}

TEST_F(CommandBusTest, CommandsAreUnregisteredByIdOrOwner) {
    register_echo();
    const auto handler = [](CommandContext&) { return CommandResult{}; };
    bus.register_command({.id = "test.other", .owner = test_owner}, handler);
    bus.register_command({.id = "else.thing", .owner = "else.module"}, handler);

    EXPECT_TRUE(bus.unregister_command("test.other"));
    EXPECT_FALSE(bus.unregister_command("test.other"));
    EXPECT_EQ(bus.unregister_owner(test_owner), 1U);
    EXPECT_EQ(bus.find_command("test.echo"), nullptr);
    EXPECT_NE(bus.find_command("else.thing"), nullptr);
    EXPECT_COMMAND_ERROR(bus.execute("test.echo", {{"text", "x"}}), CommandErrc::unknown_command);

    EXPECT_COMMAND_ERROR(bus.unregister_owner("fem.core"), CommandErrc::invalid_spec);
    EXPECT_COMMAND_ERROR(bus.unregister_command(mc::create), CommandErrc::invalid_spec);
    EXPECT_NE(bus.find_command(mc::create), nullptr);
}

TEST_F(CommandBusTest, FailedCommandChangesNothingAndIsNotJournaled) {
    std::vector<ModelChange> events;
    const fem::Subscription subscription =
        bus.on_model_changed([&](const ModelChange& change) { events.push_back(change); });
    bus.register_command(
        {.id = "test.fail", .owner = test_owner}, [](CommandContext& context) -> CommandResult {
            context.editor().create({.type = "item", .owner = test_owner, .name = "a"});
            context.editor().set_units(fem::model::UnitSystem::mm_t_s());
            throw std::logic_error("broken");
        });
    const std::string before = dump(doc);

    EXPECT_THROW(bus.execute("test.fail"), std::logic_error);
    EXPECT_EQ(dump(doc), before);
    EXPECT_FALSE(bus.can_undo());
    EXPECT_TRUE(events.empty());
    EXPECT_TRUE(journal.entries().empty());
    // The id taken by the failed command is free again, so a replayed journal gets the same ids.
    EXPECT_EQ(create("b"), ObjectId{1});
}

TEST_F(CommandBusTest, ModelErrorsPassThrough) {
    create("a");
    EXPECT_THROW(
        {
            try {
                create("a");
            } catch (const ModelError& error) {
                EXPECT_EQ(error.code(), ModelErrc::duplicate_name);
                throw;
            }
        },
        ModelError);
    EXPECT_EQ(journal.entries().size(), 1U);
}

TEST_F(CommandBusTest, CommandsRunByACommandShareItsStepAndJournalLine) {
    bus.register_command(
        {.id = "test.pair",
         .owner = test_owner,
         .args = {{.name = "name", .type = ArgType::string}}},
        [](CommandContext& context) {
            const std::string& name = context.args().string("name");
            context.execute(mc::create,
                            {{"type", "item"}, {"owner", test_owner}, {"name", name + "1"}});
            // A failed inner command can be handled; its own changes are reverted.
            try {
                context.execute(mc::create,
                                {{"type", "item"}, {"owner", test_owner}, {"name", name + "1"}});
            } catch (const ModelError&) {
            }
            return context.execute(mc::create,
                                   {{"type", "item"}, {"owner", test_owner}, {"name", name + "2"}});
        });

    const CommandResult result = bus.execute("test.pair", {{"name", "p"}});
    EXPECT_EQ(result.value, Value(2));
    EXPECT_EQ(doc.size(), 2U);
    EXPECT_EQ(journal.entries(),
              std::vector<std::string>{"stresslab.cmd(\"test.pair\", name=\"p\")"});
    EXPECT_EQ(bus.undo_label(), "test.pair");

    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(doc.size(), 0U);
    EXPECT_FALSE(bus.can_undo());
}

TEST_F(CommandBusTest, CommandMayReturnATask) {
    // The command returns at once; its work goes on in the background (task 1.6).
    fem::task::TaskPool pool(1);
    bus.register_command({.id = "test.background", .owner = test_owner}, [&](CommandContext&) {
        CommandResult result;
        result.task =
            pool.submit("background", [](fem::task::TaskControl&) -> Value { return 42; });
        return result;
    });
    const CommandResult result = bus.execute("test.background");
    ASSERT_NE(result.task, nullptr);
    EXPECT_TRUE(result.value.is_none());
    EXPECT_EQ(journal.entries(), std::vector<std::string>{"stresslab.cmd(\"test.background\")"});
    EXPECT_FALSE(bus.can_undo());  // Nothing in the model changed.
    ASSERT_TRUE(result.task->wait_for(std::chrono::seconds(10)));
    EXPECT_EQ(result.task->result(), Value(42));
}

TEST_F(CommandBusTest, EditorKeptAfterTheCommandCannotChangeTheModel) {
    fem::model::DocumentEditor* kept = nullptr;
    bus.register_command({.id = "test.keep", .owner = test_owner}, [&](CommandContext& context) {
        kept = &context.editor();
        return CommandResult{};
    });
    bus.execute("test.keep");
    ASSERT_NE(kept, nullptr);
    try {
        kept->create({.type = "item", .owner = test_owner});
        ADD_FAILURE() << "the editor changed the model outside a command";
    } catch (const ModelError& error) {
        EXPECT_EQ(error.code(), ModelErrc::outside_command);
    }
    EXPECT_EQ(doc.size(), 0U);
}

TEST_F(CommandBusTest, CommandMayUnregisterItselfWhileRunning) {
    bus.register_command({.id = "test.once", .owner = test_owner}, [&](CommandContext& context) {
        EXPECT_TRUE(bus.unregister_command("test.once"));
        return Value(context.spec().id);
    });
    EXPECT_EQ(bus.execute("test.once").value, Value("test.once"));
    EXPECT_EQ(bus.find_command("test.once"), nullptr);
}

TEST_F(CommandBusTest, HistoryAndTransactionsAreRefusedInsideACommand) {
    create("a");
    bus.register_command({.id = "test.meddle", .owner = test_owner}, [&](CommandContext&) {
        EXPECT_COMMAND_ERROR(bus.undo(), CommandErrc::invalid_state);
        EXPECT_COMMAND_ERROR(bus.redo(), CommandErrc::invalid_state);
        EXPECT_COMMAND_ERROR((void)bus.begin_transaction(), CommandErrc::invalid_state);
        return CommandResult{};
    });
    bus.execute("test.meddle");
    EXPECT_EQ(doc.size(), 1U);
}

TEST_F(CommandBusTest, ListenersMayRunCommands) {
    int reactions = 0;
    const fem::Subscription subscription = bus.on_model_changed([&](const ModelChange& change) {
        if (change.label == mc::create && reactions++ == 0) {
            bus.execute(mc::rename,
                        {{"object", change.summary.created.front()}, {"name", "renamed"}});
        }
    });
    const ObjectId id = create("a");
    EXPECT_EQ(doc.get(id).name(), "renamed");
    EXPECT_EQ(journal.entries().size(), 2U);
    EXPECT_EQ(bus.undo_label(), mc::rename);
}
