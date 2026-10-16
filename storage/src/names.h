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

}  // namespace fem::storage::detail
