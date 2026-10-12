#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Whole-file reading and writing with Unicode paths on every OS, shared by the parts of the core
// that keep JSON and TOML files (modules, profiles, settings, string catalogs). Callers turn a
// failure into the error type of their subsystem.
namespace fem::io {

/// The bytes of a file, or nothing if it cannot be opened or read.
[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& file);
/// Replaces the contents of a file; returns false if it cannot be written. The bytes go to a
/// temporary file next to it first, which then replaces the file, so a crash in the middle
/// leaves the old contents rather than a half-written file.
[[nodiscard]] bool write_file(const std::filesystem::path& file, std::string_view bytes);
/// The path as UTF-8 with '/', for messages.
[[nodiscard]] std::string display(const std::filesystem::path& path);

}  // namespace fem::io
