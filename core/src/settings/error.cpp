#include "fem/core/settings/error.h"

namespace fem::settings {

std::string_view to_string(SettingsErrc code) noexcept {
    switch (code) {
        case SettingsErrc::unknown_setting:
            return "unknown_setting";
        case SettingsErrc::wrong_type:
            return "wrong_type";
        case SettingsErrc::not_in_enum:
            return "not_in_enum";
        case SettingsErrc::below_minimum:
            return "below_minimum";
        case SettingsErrc::above_maximum:
            return "above_maximum";
        case SettingsErrc::wrong_scope:
            return "wrong_scope";
        case SettingsErrc::duplicate_setting:
            return "duplicate_setting";
        case SettingsErrc::invalid_file:
            return "invalid_file";
        case SettingsErrc::io:
            return "io";
    }
    return "unknown";
}

SettingsError::SettingsError(SettingsErrc code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

}  // namespace fem::settings
