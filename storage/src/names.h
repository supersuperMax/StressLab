#pragma once

#include <optional>
#include <string>
#include <string_view>

// Names of users and modules made into names of files and of git references that every file
// system and git accept, and back.
namespace fem::storage::detail {

/// A segment of a path in a project folder: ASCII letters, digits, '.', '_' and '-' stay, other
/// bytes are written as %XX, and so are a leading or a trailing dot and the first letter of a name
/// that Windows keeps for a device ("aux", "con.txt").
[[nodiscard]] std::string path_segment(std::string_view name);

/// A segment of a git reference for a name of a user (a variant "Композитная обшивка", a label
/// "Sent 12.10"): as `path_segment`, but text beyond ASCII stays as it is, and every dot and a
/// leading '-' are written as %XX, so that "..", ".lock", "-f" and the rules of git about them
/// never apply. References are files: libgit2 refuses paths longer than 260 characters on
/// Windows, and Cyrillic as %XX would take six of them per letter.
[[nodiscard]] std::string ref_segment(std::string_view name);

/// The name a segment was made from: %XX becomes the byte; nothing if a '%' is not followed by two
/// hex digits.
[[nodiscard]] std::optional<std::string> unescape_segment(std::string_view segment);

/// True for well-formed UTF-8 (RFC 3629: no overlong forms, no surrogates, nothing above
/// U+10FFFF).
[[nodiscard]] bool is_utf8(std::string_view text) noexcept;

/// True if `path` (from a package or a record of another computer, task 2.5) names a file inside
/// a folder on every OS: UTF-8 segments joined by '/', none empty, "." or "..", none ending with a
/// dot or a space (Windows drops them), none a name Windows keeps for a device ("con", "aux.txt"),
/// no control characters, ':' or '\\'.
[[nodiscard]] bool is_portable_relative_path(std::string_view path) noexcept;

/// The text with Latin and Cyrillic capitals made small: file systems of Windows and macOS do not
/// tell "Вариант" from "вариант", so neither do the history and packages, on any system. Other
/// scripts are compared as they are. `text` is valid UTF-8.
[[nodiscard]] std::string fold_case(std::string_view text);

}  // namespace fem::storage::detail
