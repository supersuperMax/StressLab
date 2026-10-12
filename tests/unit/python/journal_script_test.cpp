// The session journal is a Python script (task 1.8): the embedded interpreter runs it against a
// document of the application and gets the document that the parser of the core gets. Also the
// attachment of an application document to Python.

#include "fem/core/command/command_bus.h"
#include "fem/core/command/journal.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/core_keys.h"
#include "fem/python/interpreter.h"

#include "../core/command/command_test_support.h"
#include "../core/command/journal_test_support.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using fem::command::journal_header;
using fem::command::replay_journal;
using fem::command::testing::dump;
using fem::command::testing::golden_path;
using fem::command::testing::JournalSession;
using fem::command::testing::run_random_session;
using fem::command::testing::utf8;
using fem::python::Interpreter;
using fem::python::PythonError;
namespace fs = std::filesystem;

namespace {

// One interpreter for the whole test program: Python cannot start twice in a process.
Interpreter* python = nullptr;

class PythonEnvironment : public ::testing::Environment {
public:
    void SetUp() override { python = new Interpreter(); }
    void TearDown() override {
        delete python;
        python = nullptr;
    }
};

[[maybe_unused]] const ::testing::Environment* const environment =
    ::testing::AddGlobalTestEnvironment(new PythonEnvironment());

/// Lends a session to Python while it lives, as the application does.
class Attached {
public:
    explicit Attached(JournalSession& session) : core_keys_(session.bus, keys_) {
        python->attach(session.bus, keys_);
    }
    ~Attached() { python->detach(); }
    Attached(const Attached&) = delete;
    Attached& operator=(const Attached&) = delete;

private:
    fem::context::ContextKeys keys_;
    fem::context::CoreKeys core_keys_;
};

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

TEST(PythonJournal, GoldenJournalRunsAsAScript) {
    const std::string golden = read_file(golden_path());
    ASSERT_FALSE(golden.empty());

    JournalSession parsed;
    replay_journal(golden, parsed.bus);

    JournalSession scripted;
    {
        const Attached attached(scripted);
        python->run_file(golden_path());
    }
    EXPECT_EQ(dump(scripted.doc), dump(parsed.doc));
    // Run by Python, the calls are journaled again exactly as they were written.
    EXPECT_EQ(scripted.journal.file(), golden);
}

TEST(PythonJournal, HundredsOfCommandsRunAsAScriptIntoTheSameDocument) {
    JournalSession original;
    run_random_session(original.bus);
    const std::string journal = original.journal.file();

    const TempDir dir(fs::path(u8"журнал Python с пробелами"));
    const fs::path file = dir.path() / fs::path(u8"сессия 1.slpy");
    std::ofstream(file, std::ios::binary) << journal;

    JournalSession replayed;
    {
        const Attached attached(replayed);
        python->run_file(file);
    }
    EXPECT_EQ(dump(replayed.doc), dump(original.doc));
    EXPECT_EQ(replayed.journal.file(), journal);
    // The history replays too.
    while (original.bus.can_undo()) {
        ASSERT_TRUE(replayed.bus.undo());
        original.bus.undo();
        ASSERT_EQ(dump(replayed.doc), dump(original.doc));
    }
    EXPECT_FALSE(replayed.bus.can_undo());
}

TEST(PythonJournal, AnErrorInTheScriptCarriesItsTraceback) {
    JournalSession session;
    const Attached attached(session);
    try {
        python->run_string(
            utf8(u8"import stresslab\nstresslab.cmd('model.rename', object=99, "
                 u8"name='нет')\n"));
        FAIL() << "expected PythonError";
    } catch (const PythonError& error) {
        const std::string text = error.what();
        EXPECT_NE(text.find("ModelError"), std::string::npos) << text;
        EXPECT_NE(text.find("line 2"), std::string::npos) << text;
    }
    // A script that exits with code 0 has succeeded.
    EXPECT_NO_THROW(python->run_string("raise SystemExit(0)"));
    EXPECT_THROW(python->run_string("raise SystemExit(3)"), PythonError);
}

TEST(PythonJournal, PythonWorksWithTheAttachedDocument) {
    JournalSession session;
    const Attached attached(session);
    python->run_string(
        utf8(u8R"(
import stresslab
doc = stresslab.active_document()
assert doc.is_attached and doc == stresslab.attached_document()
assert stresslab.context["history.canUndo"] is False
stresslab.model.create(type="part", owner="test.module", name="Деталь")
assert stresslab.context["history.canUndo"] is True
)"));
    ASSERT_EQ(session.doc.size(), 1U);
    EXPECT_EQ(session.doc.objects().front().name(), utf8(u8"Деталь"));
    EXPECT_EQ(session.journal.text(),
              utf8(u8"stresslab.cmd(\"model.create\", type=\"part\", owner=\"test.module\", "
                   u8"name=\"Деталь\")\n"));
}

TEST(PythonJournal, CommandsWrittenInPythonRunFromTheApplication) {
    JournalSession session;
    const Attached attached(session);
    python->run_string(R"(
import stresslab
def make(ctx, name):
    return ctx.execute("model.create", type="part", owner="test.module", name=name)
stresslab.register_command("py.make", make, owner="py.module",
                           args=[stresslab.ArgSpec("name", "string")])
)");
    // The application calls it without holding the GIL; it is one step and one journal line.
    const fem::command::CommandResult result = session.bus.execute("py.make", {{"name", "p"}});
    EXPECT_EQ(result.value.as_integer(), 1);
    EXPECT_EQ(session.journal.text(), "stresslab.cmd(\"py.make\", name=\"p\")\n");
    EXPECT_TRUE(session.bus.undo());
    EXPECT_EQ(session.doc.size(), 0U);
}

TEST(PythonJournal, DetachTakesBackWhatPythonKeeps) {
    JournalSession session;
    {
        const Attached attached(session);
        python->run_string(R"(
import builtins, stresslab
builtins.kept_document = stresslab.active_document()
builtins.kept_changes = []
builtins.kept_subscription = builtins.kept_document.on_model_changed(builtins.kept_changes.append)
stresslab.register_command("py.noop", lambda ctx: None, owner="py.module")
)");
        EXPECT_NE(session.bus.find_command("py.noop"), nullptr);
    }
    // The command of Python and its listener have left the bus; Python's handle is closed.
    EXPECT_EQ(session.bus.find_command("py.noop"), nullptr);
    session.bus.execute("model.create", {{"type", "part"}, {"owner", "test.module"}});
    python->run_string(R"(
import builtins, stresslab
assert builtins.kept_changes == []
assert not builtins.kept_subscription.active
try:
    builtins.kept_document.cmd("model.create", type="part", owner="test.module")
except stresslab.ClosedError:
    pass
else:
    raise AssertionError("the document should be closed")
del builtins.kept_document, builtins.kept_changes, builtins.kept_subscription
)");
}

TEST(PythonJournal, JournalHeaderImportsTheModule) {
    // The header is all a journal needs to run: `stresslab` and `Path` are imported.
    EXPECT_NO_THROW(python->run_string(journal_header() + "assert Path and stresslab\n"));
}
