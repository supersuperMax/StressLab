#pragma once

#include "fem/core/command/value.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fem::command {

class CommandBus;

// The session journal (.slpy) is a Python script (docs/architecture.md §4.6, item 5):
//
//     # StressLab session journal
//     import stresslab
//     from pathlib import Path
//
//     stresslab.cmd("model.create", type="material", owner="fem.model.structural", name="Сталь")
//     stresslab.undo()
//     with stresslab.transaction("Move"):
//         stresslab.cmd("model.rename", object=3, name="Вал")
//
// Values are written by `to_python` (python_literal.h). The file is UTF-8 without BOM with '\n'
// line breaks. Python runs it as a script with the `stresslab` module (task 1.8, python/);
// `replay_journal` reads it without Python, with a parser that understands exactly these
// statements.

/// The lines every journal starts with, each ending with '\n', followed by an empty line.
[[nodiscard]] std::string journal_header();

/// Writes journal entries (see `CommandBus::on_journal`) to a .slpy file.
class FileJournal {
public:
    /// Creates or truncates the file and writes the header. Throws CommandError(journal_io).
    explicit FileJournal(std::filesystem::path path);

    /// Appends an entry and a line break and flushes the file, so the journal survives a crash.
    /// Does not throw: after a write error `good()` is false and later entries are dropped.
    void write(std::string_view entry) noexcept;

    [[nodiscard]] bool good() const noexcept { return out_.good(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
    std::ofstream out_;
};

/// A statement of a journal.
struct JournalStatement {
    enum class Kind : std::uint8_t { command, undo, redo, transaction };

    Kind kind = Kind::command;
    std::size_t line = 0;                ///< 1-based line in the journal text.
    std::string command;                 ///< Command id.
    Value::Dict args;                    ///< Keyword arguments of the command.
    std::string label;                   ///< Label of the transaction.
    std::vector<JournalStatement> body;  ///< Statements inside the transaction.
};

/// Parses a journal. Comments, empty lines and the imports of the header are skipped; any other
/// statement throws CommandError(journal_syntax) with its line and column.
[[nodiscard]] std::vector<JournalStatement> parse_journal(std::string_view text);

/// Runs the statements on `bus`. A failed statement throws CommandError(journal_replay) with its
/// line; a transaction around it is rolled back, earlier statements stay done.
void replay_journal(const std::vector<JournalStatement>& statements, CommandBus& bus);
void replay_journal(std::string_view text, CommandBus& bus);

}  // namespace fem::command
