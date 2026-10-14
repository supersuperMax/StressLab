// The Python console of the window in the embedded interpreter (task 1.14): lines run against the
// document of the application, commands reach its journal, Tab completes the stresslab API.

#include "fem/python/console.h"

#include "fem/core/command/journal.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/core_keys.h"
#include "fem/python/interpreter.h"

#include "../core/command/command_test_support.h"
#include "../core/command/journal_test_support.h"
#include "python_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using fem::command::SessionJournal;
using fem::command::testing::JournalSession;
using fem::command::testing::test_owner;
using fem::command::testing::utf8;
using fem::python::Console;
using fem::python::ConsoleOutput;
using fem::python::ConsoleReply;
using fem::python::testing::interpreter;

namespace {

class ConsoleTest : public ::testing::Test {
protected:
    ConsoleTest() : core_keys(session.bus, keys) { interpreter().attach(session.bus, keys); }
    ~ConsoleTest() override { interpreter().detach(); }
    ConsoleTest(const ConsoleTest&) = delete;
    ConsoleTest& operator=(const ConsoleTest&) = delete;
    ConsoleTest(ConsoleTest&&) = delete;
    ConsoleTest& operator=(ConsoleTest&&) = delete;

    JournalSession session;
    fem::context::ContextKeys keys;
    fem::context::CoreKeys core_keys;
    SessionJournal journal{session.bus};
};

TEST_F(ConsoleTest, RunsLinesAndReturnsWhatTheyPrint) {
    Console console;
    EXPECT_EQ(console.banner().rfind("Python 3.12", 0), 0U) << console.banner();

    ConsoleReply reply = console.push("6 * 7");
    EXPECT_FALSE(reply.more);
    EXPECT_EQ(reply.output, (std::vector<ConsoleOutput>{{false, "42\n"}}));

    EXPECT_TRUE(console.push("for word in ['один', 'два']:").more);
    EXPECT_TRUE(console.push("    print(word)").more);
    reply = console.push("");
    EXPECT_FALSE(reply.more);
    EXPECT_EQ(reply.output, (std::vector<ConsoleOutput>{{false, utf8(u8"один\nдва\n")}}));

    EXPECT_TRUE(console.push("if True:").more);
    console.reset();
    EXPECT_FALSE(console.push("1").more);

    reply = console.push("undefined_name");
    ASSERT_EQ(reply.output.size(), 1U);
    EXPECT_TRUE(reply.output[0].error);
    EXPECT_NE(reply.output[0].text.find("NameError"), std::string::npos);
}

TEST_F(ConsoleTest, CommandsChangeTheAttachedDocumentAndAreJournaled) {
    Console console;
    const ConsoleReply reply =
        console.push("steel = stresslab.cmd('model.create', type='material', owner='" +
                     std::string(test_owner) + "', name='" + utf8(u8"Сталь") + "')");
    EXPECT_FALSE(reply.more);
    EXPECT_TRUE(reply.output.empty()) << (reply.output.empty() ? "" : reply.output[0].text);
    ASSERT_EQ(session.doc.size(), 1U);
    ASSERT_EQ(journal.entries().size(), 1U);
    EXPECT_EQ(journal.entries()[0].rfind("stresslab.cmd(\"model.create\"", 0), 0U);

    EXPECT_FALSE(console.push("stresslab.undo()").more);
    EXPECT_EQ(session.doc.size(), 0U);
    EXPECT_EQ(journal.entries().back(), "stresslab.undo()");
}

TEST_F(ConsoleTest, CompletesTheApiOfStresslab) {
    Console console;
    const std::vector<std::string> api = console.complete("stresslab.mo");
    EXPECT_NE(std::find(api.begin(), api.end(), "stresslab.model"), api.end());
    const std::vector<std::string> commands = console.complete("x = stresslab.model.cre");
    EXPECT_EQ(commands, std::vector<std::string>{"x = stresslab.model.create("});
    EXPECT_TRUE(console.complete("no_such_thing.at").empty());
}

}  // namespace
