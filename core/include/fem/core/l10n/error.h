#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fem::l10n {

enum class L10nErrc {
    invalid_catalog,  ///< A catalog that is not JSON or not in the format of catalogs.
    io,               ///< A catalog file cannot be read.
};

[[nodiscard]] std::string_view to_string(L10nErrc code) noexcept;

/// Error raised by localization. The message is meant for developers of modules and for logs.
class L10nError : public std::runtime_error {
public:
    L10nError(L10nErrc code, const std::string& message);

    [[nodiscard]] L10nErrc code() const noexcept { return code_; }

private:
    L10nErrc code_;
};

}  // namespace fem::l10n
