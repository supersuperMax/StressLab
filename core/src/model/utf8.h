#pragma once

#include <string_view>

namespace fem::model::detail {

/// True if `text` is well-formed UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

}  // namespace fem::model::detail
