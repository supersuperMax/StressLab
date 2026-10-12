#pragma once

#include "fem/core/command/value.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace fem::command::detail {

/// Reads Python literals, names and punctuation from one line of a journal. Errors throw
/// CommandError(journal_syntax) with the line (if known) and the column, counted in characters.
class PythonReader {
public:
    /// `line` is the 1-based line of the journal, or 0 for text that is not part of a journal.
    explicit PythonReader(std::string_view text, std::size_t line = 0) noexcept
        : text_(text), line_(line) {}

    void skip_spaces() noexcept;
    /// True if only spaces and a comment are left.
    [[nodiscard]] bool at_end() noexcept;
    /// True if the next non-space character opens a string literal.
    [[nodiscard]] bool at_string() noexcept;
    /// Skips spaces and consumes `token` if it comes next.
    bool accept(std::string_view token) noexcept;
    /// Like `accept`, but fails if `token` does not come next.
    void expect(std::string_view token);
    /// The next name, [A-Za-z_][A-Za-z0-9_]*, or empty if a name does not come next.
    [[nodiscard]] std::string_view name() noexcept;

    [[nodiscard]] std::string string_literal();
    [[nodiscard]] Value value();

    [[noreturn]] void fail(std::string_view message) const;

private:
    [[nodiscard]] Value number();
    [[nodiscard]] char32_t hex_escape(int digits);

    std::string_view text_;
    std::size_t line_;
    std::size_t pos_ = 0;
};

}  // namespace fem::command::detail
