#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fem::settings {

/// Why a setting or a settings file was refused. The text shown to users is the string
/// "settings.error.<code>" of the core catalogs.
enum class SettingsErrc {
    unknown_setting,    ///< No enabled module has a setting with this key.
    wrong_type,         ///< The value is not of the type of the setting.
    not_in_enum,        ///< The value is not one of the allowed values.
    below_minimum,      ///< The number is less than the minimum.
    above_maximum,      ///< The number is greater than the maximum.
    wrong_scope,        ///< A setting of the user was given in the settings of a project.
    duplicate_setting,  ///< Another module (or the core) already has a setting with this key.
    invalid_file,       ///< A settings file that is not JSON or not in the settings format.
    io,                 ///< A settings file cannot be read or written.
};

/// "unknown_setting", "wrong_type", ...: the name of the enumerator.
[[nodiscard]] std::string_view to_string(SettingsErrc code) noexcept;

/// Error raised by settings. The message is meant for developers and logs; text shown to users
/// is produced from `code()` through localization.
class SettingsError : public std::runtime_error {
public:
    SettingsError(SettingsErrc code, const std::string& message);

    [[nodiscard]] SettingsErrc code() const noexcept { return code_; }

private:
    SettingsErrc code_;
};

}  // namespace fem::settings
