#pragma once

#include "fem/core/context/value.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fem::context::detail {

/// A node of a parsed `when` expression.
struct WhenNode {
    enum class Op : std::uint8_t {
        constant,    ///< `constant`.
        key,         ///< `key` alone.
        equals,      ///< key == literal.
        not_equals,  ///< key != literal.
        in,          ///< key in list.
        negate,      ///< !operands[0].
        all,         ///< operands[0] && operands[1] && ..., never with `all` operands.
        any,         ///< operands[0] || operands[1] || ..., never with `any` operands.
    };

    Op op = Op::constant;
    bool constant = false;
    std::string key{};       ///< The key, or the key on the left.
    std::string list{};      ///< For `in`: the key of the list.
    ContextValue literal{};  ///< For `==` and `!=`: the value.
    std::string spelling{};  ///< For `==` and `!=`: the literal as written.
    std::vector<WhenNode> operands{};
};

/// Parentheses and '!' nest at most this deep, so that parsing and evaluation cannot run out of
/// stack on an expression from a broken manifest.
inline constexpr std::size_t max_when_depth = 100;

/// Parses a `when` expression; throws WhenSyntaxError.
[[nodiscard]] WhenNode parse_when(std::string_view text);

[[nodiscard]] constexpr bool is_ascii_letter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}
[[nodiscard]] constexpr bool is_key_start(char c) noexcept {
    return is_ascii_letter(c) || c == '_';
}
[[nodiscard]] constexpr bool is_key_char(char c) noexcept {
    return is_ascii_letter(c) || is_ascii_digit(c) || c == '_' || c == '-' || c == '.';
}

}  // namespace fem::context::detail
