// The session journal (.slpy): written by the bus, read back by the parser of the core and replayed
// into a new document with an identical result. tests/unit/python/ runs the same journals as Python
// scripts.

#include "fem/core/command/journal.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
#include "fem/core/command/model_commands.h"
#include "fem/core/model/error.h"
#include "fem/core/product.h"

#include "command_test_support.h"
#include "journal_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace fem::command;
using fem::command::testing::dump;
using fem::command::testing::golden_path;
using fem::command::testing::golden_path_text;
using fem::command::testing::JournalRecorder;
using fem::command::testing::JournalSession;
using fem::command::testing::run_golden_session;
using fem::command::testing::run_random_session;
using fem::command::testing::test_owner;
using fem::command::testing::utf8;
using fem::model::Document;
namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class TempDir {
public:
    explicit TempDir(const fs::path& name) : path_(fs::temp_directory_path() / name) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

}  // namespace

TEST(Journal, HeaderMakesTheJournalAPythonScript) {
    const std::string header = journal_header();
    EXPECT_EQ(header, "# " + std::string(fem::kProductName) +
                          " session journal\nimport stresslab\nfrom pathlib import Path\n\n");
}

TEST(Journal, HundredsOfCommandsReplayIntoTheSameDocument) {
    JournalSession original;
    run_random_session(original.bus);
    const std::string journal = original.journal.file();
    std::size_t commands = 0;
    for (std::size_t at = journal.find("stresslab.cmd("); at != std::string::npos;
         at = journal.find("stresslab.cmd(", at + 1)) {
        ++commands;
    }
    EXPECT_GE(commands, 100U);
    EXPECT_NE(journal.find("stresslab.undo()"), std::string::npos);
    EXPECT_NE(journal.find("with stresslab.transaction("), std::string::npos);

    JournalSession replayed;
    replay_journal(journal, replayed.bus);
    EXPECT_EQ(dump(replayed.doc), dump(original.doc));
    EXPECT_EQ(replayed.journal.file(), journal);

    // The history replays too: both sessions undo and redo alike.
    while (original.bus.can_undo()) {
        ASSERT_TRUE(replayed.bus.undo());
        original.bus.undo();
        ASSERT_EQ(dump(replayed.doc), dump(original.doc));
    }
    EXPECT_FALSE(replayed.bus.can_undo());
}

TEST(Journal, GoldenSessionMatchesTheFile) {
    JournalSession session;
    run_golden_session(session.bus);
    const std::string golden = read_file(golden_path());
    ASSERT_FALSE(golden.empty()) << "missing " << golden_path_text();
    EXPECT_EQ(session.journal.file(), golden);

    JournalSession replayed;
    replay_journal(golden, replayed.bus);
    EXPECT_EQ(dump(replayed.doc), dump(session.doc));
    EXPECT_EQ(replayed.journal.file(), golden);
}

TEST(Journal, FileJournalWritesToAPathWithCyrillicAndSpaces) {
    const TempDir dir(fs::path(u8"журнал StressLab с пробелами"));
    const fs::path file = dir.path() / fs::path(u8"сессия 1.slpy");
    JournalSession session;
    {
        FileJournal journal(file);
        EXPECT_EQ(journal.path(), file);
        const fem::Subscription subscription =
            session.bus.on_journal([&](std::string_view entry) { journal.write(entry); });
        run_golden_session(session.bus);
        EXPECT_TRUE(journal.good());
        // Every entry is flushed at once: the file is complete while the journal is open.
        EXPECT_EQ(read_file(file), session.journal.file());
    }
    EXPECT_EQ(read_file(file), read_file(golden_path()));
}

TEST(Journal, FileJournalThatCannotBeCreatedThrows) {
    const TempDir dir(fs::path(u8"журнал без каталога"));
    EXPECT_COMMAND_ERROR((void)FileJournal(dir.path() / "missing" / "x.slpy"),
                         CommandErrc::journal_io);
}

TEST(Journal, SessionJournalKeepsTheEntriesAndSavesTheScript) {
    JournalSession session;
    SessionJournal journal(session.bus);
    EXPECT_TRUE(journal.entries().empty());
    EXPECT_EQ(journal.text(), journal_header());
    std::vector<std::string> announced;
    const fem::Subscription subscription =
        journal.on_entry([&](std::string_view entry) { announced.emplace_back(entry); });

    run_golden_session(session.bus);
    EXPECT_EQ(journal.entries(), session.journal.entries());
    EXPECT_EQ(announced, session.journal.entries());
    // A transaction is one entry of several lines.
    EXPECT_TRUE(std::any_of(
        journal.entries().begin(), journal.entries().end(),
        [](const std::string& entry) { return entry.starts_with("with stresslab.transaction("); }));
    const std::string golden = read_file(golden_path());
    EXPECT_EQ(journal.text(), golden);

    const TempDir dir(fs::path(u8"журнал сессии StressLab"));
    const fs::path file = dir.path() / fs::path(u8"мой сценарий.slpy");
    journal.save(file);
    EXPECT_EQ(read_file(file), golden);
    // Saving again replaces the file.
    session.bus.execute(
        fem::command::model_commands::rename,
        {{"object", session.doc.find_by_owner(test_owner).front()}, {"name", "renamed"}});
    journal.save(file);
    EXPECT_EQ(read_file(file), journal.text());
    EXPECT_FALSE(fs::exists(fs::path(file).concat(".tmp")));

    // The saved script rebuilds the same document.
    JournalSession replayed;
    replay_journal(read_file(file), replayed.bus);
    EXPECT_EQ(dump(replayed.doc), dump(session.doc));
}

TEST(Journal, SessionJournalThatCannotBeSavedThrows) {
    JournalSession session;
    const SessionJournal journal(session.bus);
    const TempDir dir(fs::path(u8"журнал без каталога"));
    EXPECT_COMMAND_ERROR(journal.save(dir.path() / "missing" / "x.slpy"), CommandErrc::journal_io);
}

TEST(Journal, ParserReadsStatementsAndSkipsCommentsAndHeader) {
    const std::string text =
        "# comment\r\n"
        "import stresslab\n"
        "from pathlib import Path\n"
        "\n"
        "stresslab.cmd(\"a.b\", x=1, y=Path('p/q'),)  # trailing comment\n"
        "   \n"
        "stresslab.undo()\n"
        "stresslab.redo( )\n"
        "with stresslab.transaction('t'):\n"
        "    stresslab.cmd('c.d')\n"
        "    # inside\n"
        "    with stresslab.transaction():\n"
        "        stresslab.undo()\n"
        "    stresslab.cmd(\"e.f\", z=None)\n"
        "stresslab.cmd(\"g.h\")";
    const std::vector<JournalStatement> statements = parse_journal(text);
    using Kind = JournalStatement::Kind;
    ASSERT_EQ(statements.size(), 5U);
    EXPECT_EQ(statements[0].kind, Kind::command);
    EXPECT_EQ(statements[0].line, 5U);
    EXPECT_EQ(statements[0].command, "a.b");
    EXPECT_EQ(statements[0].args, (Value::Dict{{"x", 1}, {"y", Value(fs::path("p/q"))}}));
    EXPECT_EQ(statements[1].kind, Kind::undo);
    EXPECT_EQ(statements[2].kind, Kind::redo);
    EXPECT_EQ(statements[3].kind, Kind::transaction);
    EXPECT_EQ(statements[3].label, "t");
    ASSERT_EQ(statements[3].body.size(), 3U);
    EXPECT_EQ(statements[3].body[0].command, "c.d");
    EXPECT_EQ(statements[3].body[1].kind, Kind::transaction);
    EXPECT_EQ(statements[3].body[1].label, "");
    ASSERT_EQ(statements[3].body[1].body.size(), 1U);
    EXPECT_EQ(statements[3].body[1].body[0].kind, Kind::undo);
    EXPECT_EQ(statements[3].body[2].args, (Value::Dict{{"z", Value()}}));
    EXPECT_EQ(statements[4].command, "g.h");
    EXPECT_EQ(statements[4].line, 15U);
}

TEST(Journal, ParserErrorsNameTheLine) {
    const char* bad[] = {
        "print(1)",
        "stresslab.run()",
        "stresslab.cmd()",
        "stresslab.cmd(5)",
        "stresslab.cmd(\"a.b\", 1)",
        "stresslab.cmd(\"a.b\", x=1, x=2)",
        "stresslab.cmd(\"a.b\" x=1)",
        "stresslab.undo(1)",
        "import os",
        "  stresslab.undo()",
        "with stresslab.transaction():",
        "with stresslab.transaction():\nstresslab.undo()",
        "with stresslab.transaction()\n    stresslab.undo()",
        "with stresslab.transaction():\n\tstresslab.undo()",
        "with stresslab.transaction():\n    stresslab.undo()\n  stresslab.redo()",
        "stresslab.cmd(\"a.b\", x=[1,\n2])",
    };
    for (const char* text : bad) {
        try {
            (void)parse_journal(text);
            ADD_FAILURE() << "parsed: " << text;
        } catch (const CommandError& error) {
            EXPECT_EQ(error.code(), CommandErrc::journal_syntax) << text;
            EXPECT_EQ(std::string(error.what()).rfind("line ", 0), 0U) << error.what();
        }
    }
}

TEST(Journal, ReplayErrorNamesTheLineAndKeepsEarlierStatements) {
    JournalSession session;
    const std::string text =
        journal_header() +
        "stresslab.cmd(\"model.create\", type=\"item\", owner=\"test.module\", name=\"a\")\n"
        "with stresslab.transaction():\n"
        "    stresslab.cmd(\"model.create\", type=\"item\", owner=\"test.module\", name=\"b\")\n"
        "    stresslab.cmd(\"model.create\", type=\"item\", owner=\"test.module\", name=\"a\")\n";
    try {
        replay_journal(text, session.bus);
        ADD_FAILURE() << "replayed a duplicate name";
    } catch (const CommandError& error) {
        EXPECT_EQ(error.code(), CommandErrc::journal_replay);
        EXPECT_EQ(std::string(error.what()).rfind("line 8: ", 0), 0U) << error.what();
    }
    EXPECT_EQ(session.doc.size(), 1U);  // "a" stays, the transaction with "b" is rolled back.
    EXPECT_FALSE(session.bus.in_transaction());

    EXPECT_COMMAND_ERROR(replay_journal("stresslab.redo()", session.bus),
                         CommandErrc::journal_replay);
    EXPECT_COMMAND_ERROR(replay_journal("stresslab.cmd(\"no.such\")", session.bus),
                         CommandErrc::journal_replay);
}
