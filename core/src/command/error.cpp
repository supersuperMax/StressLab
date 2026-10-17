#include "fem/core/command/error.h"

namespace fem::command {

std::string_view to_string(CommandErrc code) noexcept {
    switch (code) {
        case CommandErrc::unknown_command:
            return "unknown_command";
        case CommandErrc::duplicate_command:
            return "duplicate_command";
        case CommandErrc::invalid_spec:
            return "invalid_spec";
        case CommandErrc::invalid_arguments:
            return "invalid_arguments";
        case CommandErrc::invalid_value:
            return "invalid_value";
        case CommandErrc::invalid_state:
            return "invalid_state";
        case CommandErrc::transaction_order:
            return "transaction_order";
        case CommandErrc::journal_syntax:
            return "journal_syntax";
        case CommandErrc::journal_replay:
            return "journal_replay";
        case CommandErrc::journal_io:
            return "journal_io";
        case CommandErrc::read_only:
            return "read_only";
    }
    return "unknown";
}

CommandError::CommandError(CommandErrc code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

}  // namespace fem::command
