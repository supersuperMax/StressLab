#include "fem/core/l10n/error.h"

namespace fem::l10n {

std::string_view to_string(L10nErrc code) noexcept {
    switch (code) {
        case L10nErrc::invalid_catalog:
            return "invalid_catalog";
        case L10nErrc::io:
            return "io";
    }
    return "unknown";
}

L10nError::L10nError(L10nErrc code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

}  // namespace fem::l10n
