#include "fem/core/context/error.h"

namespace fem::context {

std::string_view to_string(ContextErrc code) noexcept {
    switch (code) {
        case ContextErrc::syntax:
            return "syntax";
        case ContextErrc::invalid_key:
            return "invalid_key";
        case ContextErrc::invalid_spec:
            return "invalid_spec";
        case ContextErrc::duplicate_key:
            return "duplicate_key";
        case ContextErrc::unknown_key:
            return "unknown_key";
        case ContextErrc::invalid_value:
            return "invalid_value";
    }
    return "unknown";
}

ContextError::ContextError(ContextErrc code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

WhenSyntaxError::WhenSyntaxError(std::size_t offset, std::size_t column, const std::string& reason)
    : ContextError(ContextErrc::syntax, "column " + std::to_string(column) + ": " + reason),
      offset_(offset),
      column_(column),
      reason_(reason) {}

}  // namespace fem::context
