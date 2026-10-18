#pragma once

// An input file as a sequence of keywords with their parameters and data lines, *INCLUDE files
// spliced in where they are included. Comments ("**") and blank lines are dropped; nothing is
// interpreted yet.

#include "inp/encoding.h"
#include "inp/problem.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fem::inp {

struct SourceFile {
    std::filesystem::path path;
    /// The file as problems name it: relative to the folder of the main file, with '/'.
    std::string display;
    std::string encoding;  ///< What the bytes were read as (DecodedText::encoding).
};

struct Parameter {
    std::string key;    ///< Upper case, inner blanks kept single: "SECTION INTEGRATION".
    std::string value;  ///< As written, without blanks around it and without quotes; may be "".
    bool has_value = false;
};

struct DataLine {
    std::int64_t line = 0;
    std::string_view text;  ///< Without the line break; points into Deck's text.
    std::size_t file = 0;   ///< Index in Deck::files: an included file may give the lines.
};

/// The fields of a data line, separated by commas, without blanks around them and without
/// quotes. A comma at the end gives an empty last field.
[[nodiscard]] std::vector<std::string_view> split_fields(std::string_view text);
/// The same into `fields`, which keeps its memory between lines.
void split_fields(std::string_view text, std::vector<std::string_view>& fields);

struct Keyword {
    std::string name;  ///< Upper case without '*', inner blanks kept single: "SOLID SECTION".
    std::vector<Parameter> params;
    std::vector<DataLine> data;
    std::size_t file = 0;   ///< Index in Deck::files.
    std::int64_t line = 0;  ///< Line of the keyword.
    /// The keyword line as written (lines joined with '\n' if it is continued).
    std::string written;
    std::size_t index = 0;  ///< Position in Deck::keywords.

    [[nodiscard]] const Parameter* find(std::string_view key) const noexcept;
    [[nodiscard]] bool has(std::string_view key) const noexcept { return find(key) != nullptr; }
    /// Value of a parameter, or nothing if the keyword does not have it.
    [[nodiscard]] std::optional<std::string> value(std::string_view key) const;
    /// "*SOLID SECTION": the name as problems show it.
    [[nodiscard]] std::string title() const { return "*" + name; }
    /// The keyword line and the data lines, as written, joined with '\n'.
    [[nodiscard]] std::string text() const;
};

struct Deck {
    std::vector<SourceFile> files;  ///< The main file first.
    std::vector<Keyword> keywords;
    std::vector<Problem> problems;  ///< Of reading, such as a file that is not UTF-8.
    std::vector<std::unique_ptr<const std::string>> texts;  ///< Decoded files the lines point into.
};

/// Reads a file and the files it includes. Throws InpError for a file that cannot be read or
/// decoded, a missing or circular *INCLUDE and data lines before the first keyword.
[[nodiscard]] Deck read_deck(const std::filesystem::path& file, Encoding encoding);

/// The text of a UTF-8 path, and back.
[[nodiscard]] std::string path_text(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path text_path(std::string_view text);

/// Upper case of ASCII letters.
[[nodiscard]] std::string upper(std::string_view text);

}  // namespace fem::inp
