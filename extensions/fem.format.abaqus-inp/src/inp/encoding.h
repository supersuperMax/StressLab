#pragma once

// Text encodings of input files. Abaqus reads ASCII, but files carry comments and headings in
// whatever the editor wrote: UTF-8 (with or without a BOM), UTF-16 from Windows editors, or a
// single-byte code page. The model holds UTF-8 only.

#include <optional>
#include <string>
#include <string_view>

namespace fem::inp {

enum class Encoding { automatic, utf8, windows1251, windows1252, latin1 };

/// "auto", "utf-8", "windows-1251", "windows-1252", "latin-1" (ASCII case does not matter).
[[nodiscard]] std::optional<Encoding> parse_encoding(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(Encoding encoding) noexcept;

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

struct DecodedText {
    std::string text;  ///< UTF-8, without a byte order mark.
    /// What the bytes were read as: "utf-8", "utf-16le", "utf-16be" or a code page.
    std::string encoding;
    /// The bytes were not UTF-8 and `automatic` fell back to Windows-1252.
    bool fallback = false;
};

/// Reads the bytes of a file as text. A byte order mark wins over `requested`. Without one,
/// `automatic` takes UTF-8 if the bytes are valid UTF-8 and Windows-1252 otherwise; `utf8`
/// refuses invalid bytes. Returns nothing if the bytes are not valid in the encoding.
[[nodiscard]] std::optional<DecodedText> decode(std::string_view bytes, Encoding requested);

}  // namespace fem::inp
