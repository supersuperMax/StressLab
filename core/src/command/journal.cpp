#include "fem/core/command/journal.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
#include "fem/core/product.h"

#include "python_reader.h"

#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace fem::command {

namespace {

struct Line {
    std::size_t number;  // 1-based
    std::size_t indent;
    std::string_view text;
};

[[noreturn]] void fail_at(std::size_t line, const std::string& message) {
    throw CommandError(CommandErrc::journal_syntax,
                       "line " + std::to_string(line) + ": " + message);
}

/// Lines with statements: without empty lines and comment lines.
std::vector<Line> statement_lines(std::string_view text) {
    std::vector<Line> lines;
    std::size_t number = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::size_t indent = line.find_first_not_of(' ');
        if (indent == std::string_view::npos) {
            continue;
        }
        if (line[indent] == '\t') {
            fail_at(number, "tabs are not allowed in indentation");
        }
        if (line[indent] == '#') {
            continue;
        }
        lines.push_back({number, indent, line});  // With indentation: columns count from 1.
    }
    return lines;
}

/// The statement on `line`, or nothing for the imports of the header.
std::optional<JournalStatement> parse_statement(const Line& line) {
    detail::PythonReader reader(line.text, line.number);
    JournalStatement statement;
    statement.line = line.number;
    const auto finish = [&]() -> std::optional<JournalStatement> {
        if (!reader.at_end()) {
            reader.fail("unexpected text after the statement");
        }
        return statement;
    };

    const std::string_view word = reader.name();
    if (word == "import") {
        if (reader.name() != "stresslab" || !reader.at_end()) {
            reader.fail("the journal imports only stresslab");
        }
        return std::nullopt;
    }
    if (word == "from") {
        if (reader.name() != "pathlib" || reader.name() != "import" || reader.name() != "Path" ||
            !reader.at_end()) {
            reader.fail("the journal imports only Path from pathlib");
        }
        return std::nullopt;
    }
    if (word == "with") {
        if (reader.name() != "stresslab" || !reader.accept(".") || reader.name() != "transaction") {
            reader.fail("expected 'with stresslab.transaction(...):'");
        }
        statement.kind = JournalStatement::Kind::transaction;
        reader.expect("(");
        if (reader.at_string()) {
            statement.label = reader.string_literal();
            reader.accept(",");
        }
        reader.expect(")");
        reader.expect(":");
        return finish();
    }
    if (word != "stresslab" || !reader.accept(".")) {
        reader.fail(
            "unsupported statement: until the Python API exists, the journal may contain only "
            "stresslab.cmd(), stresslab.undo(), stresslab.redo() and 'with "
            "stresslab.transaction():' blocks");
    }

    const std::string_view function = reader.name();
    if (function == "undo" || function == "redo") {
        statement.kind =
            function == "undo" ? JournalStatement::Kind::undo : JournalStatement::Kind::redo;
        reader.expect("(");
        reader.expect(")");
        return finish();
    }
    if (function != "cmd") {
        reader.fail("unknown function stresslab." + std::string(function));
    }
    statement.kind = JournalStatement::Kind::command;
    reader.expect("(");
    if (!reader.at_string()) {
        reader.fail("expected the command id in quotes");
    }
    statement.command = reader.string_literal();
    while (!reader.accept(")")) {
        reader.expect(",");
        if (reader.accept(")")) {
            break;
        }
        const std::string_view name = reader.name();
        if (name.empty()) {
            reader.fail("expected a keyword argument");
        }
        reader.expect("=");
        if (!statement.args.emplace(std::string(name), reader.value()).second) {
            reader.fail("keyword argument '" + std::string(name) + "' is repeated");
        }
    }
    return finish();
}

std::vector<JournalStatement> parse_block(const std::vector<Line>& lines, std::size_t& next,
                                          std::size_t indent) {
    std::vector<JournalStatement> statements;
    while (next < lines.size() && lines[next].indent >= indent) {
        const Line& line = lines[next];
        if (line.indent > indent) {
            fail_at(line.number, "unexpected indentation");
        }
        std::optional<JournalStatement> statement = parse_statement(line);
        ++next;
        if (!statement) {
            continue;
        }
        if (statement->kind == JournalStatement::Kind::transaction) {
            if (next == lines.size() || lines[next].indent <= indent) {
                fail_at(line.number, "expected an indented block after 'with'");
            }
            statement->body = parse_block(lines, next, lines[next].indent);
            if (next < lines.size() && lines[next].indent > indent) {
                fail_at(lines[next].number, "the indentation does not match any outer block");
            }
        }
        statements.push_back(std::move(*statement));
    }
    return statements;
}

/// Runs one statement; errors get its line.
template <class Action>
void run_statement(const JournalStatement& statement, Action&& action) {
    const auto wrap = [&](const char* what) {
        return CommandError(CommandErrc::journal_replay,
                            "line " + std::to_string(statement.line) + ": " + what);
    };
    try {
        action();
    } catch (const CommandError& error) {
        if (error.code() == CommandErrc::journal_replay) {
            throw;
        }
        throw wrap(error.what());
    } catch (const std::exception& error) {
        throw wrap(error.what());
    }
}

}  // namespace

std::string journal_header() {
    return "# " + std::string(kProductName) +
           " session journal\nimport stresslab\nfrom pathlib import Path\n\n";
}

// --- FileJournal ---------------------------------------------------------------------------------

FileJournal::FileJournal(std::filesystem::path path)
    : path_(std::move(path)), out_(path_, std::ios::binary | std::ios::trunc) {
    const std::string header = journal_header();
    out_.write(header.data(), static_cast<std::streamsize>(header.size()));
    out_.flush();
    if (!out_) {
        const std::u8string name = path_.u8string();
        throw CommandError(CommandErrc::journal_io,
                           "cannot write the journal " + std::string(name.begin(), name.end()));
    }
}

void FileJournal::write(std::string_view entry) noexcept {
    if (!out_) {
        return;
    }
    out_.write(entry.data(), static_cast<std::streamsize>(entry.size()));
    out_.put('\n');
    out_.flush();
}

// --- Reading and replay --------------------------------------------------------------------------

std::vector<JournalStatement> parse_journal(std::string_view text) {
    const std::vector<Line> lines = statement_lines(text);
    std::size_t next = 0;
    return parse_block(lines, next, 0);
}

void replay_journal(const std::vector<JournalStatement>& statements, CommandBus& bus) {
    for (const JournalStatement& statement : statements) {
        switch (statement.kind) {
            case JournalStatement::Kind::command:
                run_statement(statement, [&] { bus.execute(statement.command, statement.args); });
                break;
            case JournalStatement::Kind::undo:
                run_statement(statement, [&] {
                    if (!bus.undo()) {
                        throw CommandError(CommandErrc::invalid_state, "nothing to undo");
                    }
                });
                break;
            case JournalStatement::Kind::redo:
                run_statement(statement, [&] {
                    if (!bus.redo()) {
                        throw CommandError(CommandErrc::invalid_state, "nothing to redo");
                    }
                });
                break;
            case JournalStatement::Kind::transaction: {
                std::optional<Transaction> transaction;
                run_statement(statement,
                              [&] { transaction.emplace(bus.begin_transaction(statement.label)); });
                replay_journal(statement.body, bus);
                run_statement(statement, [&] { transaction->commit(); });
                break;
            }
        }
    }
}

void replay_journal(std::string_view text, CommandBus& bus) {
    replay_journal(parse_journal(text), bus);
}

}  // namespace fem::command
