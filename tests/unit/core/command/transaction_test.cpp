// Transactions: a group of commands is one undo step and one journal entry.

#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
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
using fem::model::ModelError;
using fem::model::ObjectId;
namespace mc = fem::command::model_commands;

namespace {

class TransactionTest : public ::testing::Test {
protected:
    void SetUp() override { doc.set_owner_enabled(test_owner, true); }

    ObjectId create(const std::string& name) {
        return ObjectId(static_cast<std::uint64_t>(
            bus.execute(mc::create, {{"type", "item"}, {"owner", test_owner}, {"name", name}})
                .value.as_integer()));
    }

    static std::string create_line(const std::string& name) {
        return "stresslab.cmd(\"model.create\", type=\"item\", owner=\"test.module\", name=\"" +
               name + "\")";
    }

    Document doc;
    CommandBus bus{doc};
    JournalRecorder journal{bus};
    std::vector<ModelChange> events;
    fem::Subscription subscription =
        bus.on_model_changed([this](const ModelChange& change) { events.push_back(change); });
};

}  // namespace

TEST_F(TransactionTest, CommittedTransactionIsOneStepAndOneEntry) {
    const std::string before = dump(doc);
    Transaction transaction = bus.begin_transaction(utf8(u8"Сборка"));
    EXPECT_TRUE(bus.in_transaction());
    const ObjectId a = create("a");
    const ObjectId b = create("b");
    bus.execute(mc::add_dependency, {{"object", b}, {"target", a}});
    EXPECT_TRUE(journal.entries().empty());
    EXPECT_TRUE(events.empty());
    EXPECT_FALSE(bus.can_undo());

    transaction.commit();
    EXPECT_FALSE(transaction.is_open());
    EXPECT_FALSE(bus.in_transaction());
    const std::string after = dump(doc);

    ASSERT_EQ(journal.entries().size(), 1U);
    EXPECT_EQ(journal.entries()[0],
              utf8(u8"with stresslab.transaction(\"Сборка\"):\n    ") + create_line("a") +
                  "\n    " + create_line("b") +
                  "\n    stresslab.cmd(\"model.add_dependency\", object=2, target=1)");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].label, utf8(u8"Сборка"));
    EXPECT_EQ(events[0].summary.created, (std::vector<ObjectId>{a, b}));
    EXPECT_EQ(bus.undo_label(), utf8(u8"Сборка"));

    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(dump(doc), before);
    EXPECT_FALSE(bus.can_undo());
    ASSERT_TRUE(bus.redo());
    EXPECT_EQ(dump(doc), after);
}

TEST_F(TransactionTest, TransactionWithoutLabel) {
    Transaction transaction = bus.begin_transaction();
    create("a");
    transaction.commit();
    EXPECT_EQ(journal.entries(),
              std::vector<std::string>{"with stresslab.transaction():\n    " + create_line("a")});
    EXPECT_EQ(bus.undo_label(), "");
}

TEST_F(TransactionTest, RollbackRevertsEverythingAndForgetsIt) {
    create("kept");
    const std::string before = dump(doc);
    events.clear();
    {
        Transaction transaction = bus.begin_transaction("t");
        create("a");
        bus.execute(mc::set_units,
                    {{"length", "mm"}, {"mass", "t"}, {"time", "s"}, {"temperature", "K"}});
        transaction.rollback();
        EXPECT_FALSE(transaction.is_open());
        transaction.rollback();  // Closed already: nothing happens.
    }
    EXPECT_EQ(dump(doc), before);
    EXPECT_EQ(journal.entries().size(), 1U);
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(bus.undo_label(), mc::create);
    EXPECT_EQ(create("b"), ObjectId{2});  // The id of the rolled back object is free again.
}

TEST_F(TransactionTest, DestroyedOpenTransactionRollsBack) {
    {
        Transaction transaction = bus.begin_transaction();
        create("a");
    }
    EXPECT_EQ(doc.size(), 0U);
    EXPECT_FALSE(bus.in_transaction());
    EXPECT_TRUE(journal.entries().empty());
    EXPECT_FALSE(bus.can_undo());
}

TEST_F(TransactionTest, MovedTransactionStaysOpen) {
    Transaction outer = bus.begin_transaction();
    Transaction moved = std::move(outer);
    EXPECT_FALSE(
        outer.is_open());  // NOLINT(bugprone-use-after-move): checking the moved-from state.
    EXPECT_TRUE(moved.is_open());
    create("a");
    moved.commit();
    EXPECT_TRUE(bus.can_undo());
}

TEST_F(TransactionTest, InnerCommitJoinsTheOuterTransaction) {
    Transaction outer = bus.begin_transaction("outer");
    create("a");
    {
        Transaction inner = bus.begin_transaction("inner");
        create("b");
        inner.commit();
    }
    create("c");
    EXPECT_TRUE(journal.entries().empty());
    outer.commit();

    // The journal flattens nested transactions: only the outer one is a step.
    EXPECT_EQ(
        journal.entries(),
        std::vector<std::string>{"with stresslab.transaction(\"outer\"):\n    " + create_line("a") +
                                 "\n    " + create_line("b") + "\n    " + create_line("c")});
    ASSERT_TRUE(bus.undo());
    EXPECT_EQ(doc.size(), 0U);
    EXPECT_FALSE(bus.can_undo());
}

TEST_F(TransactionTest, InnerRollbackKeepsTheOuterTransaction) {
    Transaction outer = bus.begin_transaction("outer");
    create("a");
    {
        Transaction inner = bus.begin_transaction("inner");
        create("b");
    }
    const ObjectId c = create("c");
    outer.commit();

    EXPECT_EQ(c, ObjectId{2});
    EXPECT_EQ(doc.size(), 2U);
    EXPECT_FALSE(doc.find_child({}, "item", "b").is_valid());
    EXPECT_EQ(journal.entries(),
              std::vector<std::string>{"with stresslab.transaction(\"outer\"):\n    " +
                                       create_line("a") + "\n    " + create_line("c")});
}

TEST_F(TransactionTest, OuterRollbackClosesInnerTransactions) {
    Transaction outer = bus.begin_transaction();
    create("a");
    Transaction inner = bus.begin_transaction();
    create("b");
    outer.rollback();
    EXPECT_FALSE(inner.is_open());
    EXPECT_FALSE(bus.in_transaction());
    EXPECT_EQ(doc.size(), 0U);
    EXPECT_COMMAND_ERROR(inner.commit(), CommandErrc::invalid_state);
}

TEST_F(TransactionTest, OuterCannotCommitWhileInnerIsOpen) {
    Transaction outer = bus.begin_transaction();
    Transaction inner = bus.begin_transaction();
    create("a");
    EXPECT_COMMAND_ERROR(outer.commit(), CommandErrc::transaction_order);
    EXPECT_TRUE(outer.is_open());
    inner.commit();
    outer.commit();
    EXPECT_EQ(doc.size(), 1U);
    EXPECT_COMMAND_ERROR(outer.commit(), CommandErrc::invalid_state);
}

TEST_F(TransactionTest, UndoAndRedoAreRefusedInsideATransaction) {
    create("a");
    Transaction transaction = bus.begin_transaction();
    EXPECT_COMMAND_ERROR(bus.undo(), CommandErrc::invalid_state);
    EXPECT_COMMAND_ERROR(bus.redo(), CommandErrc::invalid_state);
    transaction.commit();
    EXPECT_TRUE(bus.undo());
}

TEST_F(TransactionTest, FailedCommandInsideKeepsTheTransactionOpen) {
    Transaction transaction = bus.begin_transaction();
    create("a");
    EXPECT_THROW(create("a"), ModelError);
    EXPECT_TRUE(transaction.is_open());
    create("b");
    transaction.commit();

    EXPECT_EQ(doc.size(), 2U);
    EXPECT_EQ(journal.entries(),
              std::vector<std::string>{"with stresslab.transaction():\n    " + create_line("a") +
                                       "\n    " + create_line("b")});
}

TEST_F(TransactionTest, TransactionWithoutModelChangesIsJournaledButIsNoStep) {
    bus.register_command({.id = "test.noop", .owner = test_owner},
                         [](CommandContext&) { return CommandResult{}; });
    create("a");
    ASSERT_TRUE(bus.undo());
    {
        Transaction transaction = bus.begin_transaction();
        bus.execute("test.noop");
        transaction.commit();
    }
    EXPECT_TRUE(bus.can_redo());  // No new step, so redo is still possible.
    EXPECT_EQ(journal.entries().back(),
              "with stresslab.transaction():\n    stresslab.cmd(\"test.noop\")");
    {
        Transaction empty = bus.begin_transaction("empty");
        empty.commit();
    }
    EXPECT_EQ(journal.entries().size(), 3U);  // An empty transaction writes nothing.
}

TEST_F(TransactionTest, CommittedTransactionClearsRedo) {
    create("a");
    ASSERT_TRUE(bus.undo());
    Transaction transaction = bus.begin_transaction();
    create("b");
    EXPECT_TRUE(bus.can_redo());  // Not yet: the step is not complete.
    transaction.commit();
    EXPECT_FALSE(bus.can_redo());
}
