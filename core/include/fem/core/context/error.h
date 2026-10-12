#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fem::context {

/// What went wrong with context keys or a `when` expression.
enum class ContextErrc {
    syntax,         ///< A `when` expression cannot be parsed; the error is a WhenSyntaxError.
    invalid_key,    ///< A key name that the `when` language cannot read (see `is_valid_key`).
    invalid_spec,   ///< Bad declaration of a key (owner, type, initial value) or empty listener.
    duplicate_key,  ///< The key is already declared with another spec.
    unknown_key,    ///< The key is not declared.
    invalid_value,  ///< A value of another kind than the key, a non-finite number or bad UTF-8.
};

[[nodiscard]] std::string_view to_string(ContextErrc code) noexcept;

/// Error raised by context keys and `when` expressions. The message is meant for developers of
/// modules and for logs; text shown to users is produced from `code()` through localization.
class ContextError : public std::runtime_error {
public:
    ContextError(ContextErrc code, const std::string& message);

    [[nodiscard]] ContextErrc code() const noexcept { return code_; }

private:
    ContextErrc code_;
};

/// A `when` expression that cannot be parsed. `what()` is "column N: reason".
class WhenSyntaxError : public ContextError {
public:
    WhenSyntaxError(std::size_t offset, std::size_t column, const std::string& reason);

    /// Byte offset of the error in the expression; its length if the error is at the end.
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    /// 1-based column of the error, counted in characters (code points), not bytes.
    [[nodiscard]] std::size_t column() const noexcept { return column_; }
    /// What is wrong, without the column.
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

private:
    std::size_t offset_;
    std::size_t column_;
    std::string reason_;
};

}  // namespace fem::context
