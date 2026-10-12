#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace fem::extension::detail {

/// The bytes of a file. Throws ExtensionError(io).
[[nodiscard]] std::string read_file(const std::filesystem::path& file);
/// Replaces the contents of a file. Throws ExtensionError(io).
void write_file(const std::filesystem::path& file, std::string_view bytes);
/// The path as UTF-8 with '/', for messages.
[[nodiscard]] std::string display(const std::filesystem::path& path);

}  // namespace fem::extension::detail
