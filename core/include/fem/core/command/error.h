#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fem::command {

/// What went wrong in a call to the Command Bus. Errors of the model itself are ModelError and
/// pass through the bus unchanged.
enum class CommandErrc {
    unknown_command,    ///< No command with this id is registered.
    duplicate_command,  ///< A command with this id is already registered.
    invalid_spec,       ///< Bad command id, owner or argument schema.
    invalid_arguments,  ///< The arguments do not match the schema of the command.
    invalid_value,      ///< A value that the journal cannot hold: non-finite number, bad UTF-8.
    invalid_state,      ///< Undo, redo or a transaction where they are not allowed.
    transaction_order,  ///< Commit of a transaction while a transaction inside it is open.
    journal_syntax,     ///< The journal is not in the format the core writes.
    journal_replay,     ///< A statement of the journal failed when it was replayed.
    journal_io,         ///< The journal file cannot be created.
};

[[nodiscard]] std::string_view to_string(CommandErrc code) noexcept;

/// Error raised by the Command Bus. The message is meant for developers and logs; text shown to
/// users is produced from `code()` through localization.
class CommandError : public std::runtime_error {
public:
    CommandError(CommandErrc code, const std::string& message);

    [[nodiscard]] CommandErrc code() const noexcept { return code_; }

private:
    CommandErrc code_;
};

}  // namespace fem::command
