#pragma once

#include "fem/core/command/value.h"

#include <string>
#include <string_view>

namespace fem::command {

// The journal writes values as Python literals. The text depends only on the value, so the
// same call always gives the same line (CLAUDE.md, rule 6):
//
// - None, True, False; integers in decimal;
// - numbers as Python's repr(): the shortest text that reads back to the same double, with an
//   exponent below 1e-4 and from 1e16 on, and ".0" after whole values: 0.1, 1e-05, 210000.0, 1e+16;
//   NaN and infinities are refused;
// - text in double quotes, UTF-8 as is; escaped are \\ \" \n \r \t, other control characters and
//   U+2028, U+2029 (so that no tool splits the line), as \xHH or \uHHHH;
// - paths as Path("...") in generic form, with '/' as separator on every OS;
// - lists as [a, b]; dictionaries as {"key": value} with keys in byte order.

/// Writes `value` as a Python expression. Throws CommandError(invalid_value) for NaN, infinities
/// and text or paths that are not valid UTF-8.
[[nodiscard]] std::string to_python(const Value& value);

/// A Python string literal for UTF-8 `text`. Throws CommandError(invalid_value) if the text is
/// not valid UTF-8.
[[nodiscard]] std::string python_string(std::string_view text);

/// Reads a Python literal in the form `to_python` writes (single-quoted strings, spaces and
/// trailing commas are accepted too). Throws CommandError(journal_syntax) with the column.
[[nodiscard]] Value parse_python(std::string_view text);

}  // namespace fem::command
