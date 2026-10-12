// The session journal (.slpy): written by the bus, read back by the parser stub (until the Python
// API of task 1.8) and replayed into a new document with an identical result.

#include "fem/core/command/journal.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
#include "fem/core/command/model_commands.h"
#include "fem/core/model/error.h"
#include "fem/core/product.h"

#include "command_test_support.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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
namespace fs = std::filesystem;
namespace mc = fem::command::model_commands;

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

/// A document with its bus, a module that is enabled and its commands, as a session would have.
struct Session {
    Session() {
        doc.set_owner_enabled(test_owner, true);
        // A command of a module with a path argument: creates an object named after the file.
        bus.register_command(
            {.id = "test.load",
             .owner = test_owner,
             .args = {{.name = "file", .type = ArgType::path},
                      {.name = "options", .type = ArgType::dict, .required = false}}},
            [](CommandContext& context) {
                fem::model::NewObject init{.type = "import", .owner = test_owner};
                const std::u8string stem = context.args().path("file").stem().u8string();
                init.name.assign(stem.begin(), stem.end());
                if (const Value* options = context.args().find("options")) {
                    for (const auto& [key, value] : options->as_dict()) {
                        init.properties[key] = value.as_number();
                    }
                }
                return Value(context.editor().create(std::move(init)));
            });
    }

    Document doc;
    CommandBus bus{doc};
    JournalRecorder journal{bus};
};

ObjectId to_id(const CommandResult& result) {
    return ObjectId(static_cast<std::uint64_t>(result.value.as_integer()));
}

/// The session behind tests/data/journal/golden.slpy.
void run_golden_session(CommandBus& bus) {
    bus.execute(mc::set_units,
                {{"length", "mm"}, {"mass", "t"}, {"time", "s"}, {"temperature", "degC"}});
    const ObjectId steel = to_id(bus.execute(
        mc::create,
        {{"type", "material"},
         {"owner", test_owner},
         {"name", utf8(u8"Сталь «Ст3»")},
         {"properties", Value::Dict{{"E", 210000.0},
                                    {"nu", 0.3},
                                    {"rho", 7.85e-9},
                                    {"alpha", 1.2e-5},
                                    {"table", Value::List{Value::List{20.0, 210000.0},
                                                          Value::List{400.0, 170000.0}}},
                                    {"note", utf8(u8"ГОСТ 380-2005\n\"кипящая\"\tсталь")}}}}));
    const ObjectId part = to_id(bus.execute(
        mc::create, {{"type", "part"}, {"owner", test_owner}, {"name", utf8(u8"Кронштейн")}}));
    const ObjectId load = to_id(bus.execute(
        mc::create, {{"type", "load"},
                     {"owner", test_owner},
                     {"name", "P1"},
                     {"parent", part},
                     {"dependencies", Value::List{steel}},
                     {"properties",
                      Value::Dict{{"value", -1500}, {"direction", Value::List{0.0, 0.0, -1.0}}}}}));
    bus.execute(mc::rename, {{"object", load}, {"name", utf8(u8"Сила 1")}});
    bus.undo();
    bus.redo();
    ObjectId pressure;
    {
        Transaction transaction = bus.begin_transaction(utf8(u8"Нагрузки"));
        pressure = to_id(bus.execute(
            mc::create,
            {{"type", "load"}, {"owner", test_owner}, {"name", "p"}, {"parent", part}}));
        bus.execute(mc::set_properties,
                    {{"object", pressure},
                     {"properties", Value::Dict{{"value", 0.5}, {"empty", Value::Dict{}}}}});
        bus.execute(mc::add_dependency, {{"object", pressure}, {"target", steel}});
        transaction.commit();
    }
    {
        Transaction abandoned = bus.begin_transaction("abandoned");
        bus.execute(mc::create, {{"type", "load"}, {"owner", test_owner}, {"name", "lost"}});
    }
    bus.execute("test.load", {{"file", utf8(u8"D:/Проекты/модель 1.dat")},
                              {"options", Value::Dict{{"scale", 2}}}});
    bus.execute(mc::remove, {{"object", load}});
    bus.execute(mc::remove_dependency, {{"object", pressure}, {"target", steel}});
}

std::string golden_path_text() {
    return FEM_TEST_DATA_DIR "/journal/golden.slpy";
}

fs::path golden_path() {
    const std::string text = golden_path_text();
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// Deterministic pseudo-random numbers, the same on every platform.
class Sequence {
public:
    std::uint32_t next(std::uint32_t bound) {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33) % bound;
    }

private:
    std::uint64_t state_ = 2026;
};

/// Runs a long random session of the core commands, undo, redo and transactions; some commands
/// fail (duplicate names, cycles, removed objects) and some transactions are rolled back.
void run_random_session(CommandBus& bus) {
    Sequence random;
    std::vector<ObjectId> objects;
    int completed = 0;
    const char* names[] = {"a", "b", "балка", "узел 1", "Сталь «Ст3»", "x\"y", "tab\there", "😀"};
    const char* units[][4] = {
        {"m", "kg", "s", "K"}, {"mm", "t", "s", "degC"}, {"in", "slinch", "s", "degF"}};

    const auto pick = [&]() -> ObjectId {
        return objects.empty() ? ObjectId{}
                               : objects[random.next(static_cast<std::uint32_t>(objects.size()))];
    };
    const auto random_value = [&](int depth) {
        const auto make = [&](auto& self, int level) -> Value {
            switch (random.next(level > 1 ? 5 : 7)) {
                case 0:
                    return Value(static_cast<int>(random.next(2000)) - 1000);
                case 1:
                    return Value(static_cast<double>(random.next(100000)) * 1e-3);
                case 2:
                    return Value(static_cast<double>(random.next(1000)) * 1.5e-7);
                case 3:
                    return Value(names[random.next(8)]);
                case 4:
                    return Value(random.next(2) == 1);
                case 5: {
                    Value::List list;
                    for (std::uint32_t i = random.next(4); i > 0; --i) {
                        list.push_back(self(self, level + 1));
                    }
                    return Value(std::move(list));
                }
                default: {
                    Value::Dict dict;
                    for (std::uint32_t i = random.next(4); i > 0; --i) {
                        dict[names[random.next(8)]] = self(self, level + 1);
                    }
                    return Value(std::move(dict));
                }
            }
        };
        return make(make, depth);
    };
    const auto properties = [&] {
        Value::Dict dict;
        for (std::uint32_t i = random.next(4); i > 0; --i) {
            dict[std::string("p") + std::to_string(random.next(5))] = random_value(0);
        }
        return Value(std::move(dict));
    };

    const auto run_one = [&] {
        Value::Dict args;
        std::string command;
        switch (random.next(9)) {
            case 0:
            case 1:
            case 2: {
                command = mc::create;
                args = {
                    {"type", random.next(2) == 1 ? "item" : "group"},
                    {"owner", test_owner},
                    {"name", std::string(names[random.next(8)]) + std::to_string(random.next(3))},
                    {"properties", properties()}};
                if (const ObjectId parent = pick(); parent.is_valid() && random.next(2) == 1) {
                    args["parent"] = parent;
                }
                if (const ObjectId target = pick(); target.is_valid() && random.next(3) == 1) {
                    args["dependencies"] = Value::List{target};
                }
                break;
            }
            case 3:
                command = mc::rename;
                args = {
                    {"object", pick()},
                    {"name", std::string(names[random.next(8)]) + std::to_string(random.next(3))}};
                break;
            case 4:
                command = mc::set_properties;
                args = {{"object", pick()}, {"properties", properties()}};
                break;
            case 5:
                command = random.next(2) == 1 ? mc::add_dependency : mc::remove_dependency;
                args = {{"object", pick()}, {"target", pick()}};
                break;
            case 6:
                command = mc::remove;
                args = {{"object", pick()}};
                break;
            case 7: {
                command = mc::set_units;
                const auto& system = units[random.next(3)];
                args = {{"length", system[0]},
                        {"mass", system[1]},
                        {"time", system[2]},
                        {"temperature", system[3]}};
                break;
            }
            default:
                if (bus.in_transaction()) {
                    return;
                }
                if (random.next(2) == 1) {
                    bus.undo();
                } else {
                    bus.redo();
                }
                return;
        }
        if (args.contains("object") && args.at("object").as_integer() == 0) {
            return;  // No objects yet.
        }
        try {
            const CommandResult result = bus.execute(command, args);
            ++completed;
            if (command == mc::create) {
                objects.push_back(to_id(result));
            }
        } catch (const ModelError&) {
            // Duplicate names, cycles, removed objects: refused, not journaled.
        }
    };

    while (completed < 150) {
        if (random.next(8) == 0) {
            Transaction transaction = bus.begin_transaction(names[random.next(8)]);
            for (std::uint32_t i = 2 + random.next(3); i > 0; --i) {
                run_one();
            }
            if (random.next(4) != 0) {
                transaction.commit();
            }
        } else {
            run_one();
        }
    }
}

}  // namespace

TEST(Journal, HeaderMakesTheJournalAPythonScript) {
    const std::string header = journal_header();
    EXPECT_EQ(header, "# " + std::string(fem::kProductName) +
                          " session journal\nimport stresslab\nfrom pathlib import Path\n\n");
}

TEST(Journal, HundredsOfCommandsReplayIntoTheSameDocument) {
    Session original;
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

    Session replayed;
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
    Session session;
    run_golden_session(session.bus);
    const std::string golden = read_file(golden_path());
    ASSERT_FALSE(golden.empty()) << "missing " << golden_path_text();
    EXPECT_EQ(session.journal.file(), golden);

    Session replayed;
    replay_journal(golden, replayed.bus);
    EXPECT_EQ(dump(replayed.doc), dump(session.doc));
    EXPECT_EQ(replayed.journal.file(), golden);
}

TEST(Journal, FileJournalWritesToAPathWithCyrillicAndSpaces) {
    const TempDir dir(fs::path(u8"журнал StressLab с пробелами"));
    const fs::path file = dir.path() / fs::path(u8"сессия 1.slpy");
    Session session;
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
    Session session;
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
