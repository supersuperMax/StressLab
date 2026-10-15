#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

// JSON of project files (docs/architecture.md §3.1): one canonical text per value, so that saving
// what was loaded gives the same bytes and diffs show only real changes (CLAUDE.md, rule 6).
namespace fem::storage {

/// The canonical text of `value`:
/// - keys of objects sorted by their UTF-8 bytes, which is code point order;
/// - two spaces of indentation per level, one member or item per line, `": "` after keys,
///   `{}` and `[]` for empty ones, `\n` line breaks and one at the end;
/// - text as UTF-8, escaping only `"`, `\` and control characters (`\n`, `\t`, `\u001f`...);
/// - integers in decimal; other numbers in the shortest form that reads back to the same double,
///   always with a point or an exponent (`fem::text::format_double`), so 1.0 stays a float.
/// Throws ProjectError(invalid_data) for NaN, infinities, invalid UTF-8 and binary values.
[[nodiscard]] std::string to_canonical_json(const nlohmann::json& value);

/// Reads a JSON text, also one with a UTF-8 byte order mark or `\r\n` line breaks (a git
/// checkout on Windows may make them). Throws ProjectError(invalid_file) with `file` and the line
/// of the error.
[[nodiscard]] nlohmann::json parse_json(std::string_view text, const std::string& file);

}  // namespace fem::storage
