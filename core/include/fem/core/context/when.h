#pragma once

#include "fem/core/context/value.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fem::context {

namespace detail {
struct WhenNode;
}  // namespace detail

/// True if `name` can be a context key: names of letters, digits, '_' and '-' joined by single
/// dots, starting with a letter or '_' ("model.hasMesh", "solver.my-solver.available"), except
/// the words of the language: true, false and in. Keys are case-sensitive.
[[nodiscard]] bool is_valid_key(std::string_view name) noexcept;

/// A part of a false expression that would have to become true: what the shell names when it says
/// why a command is unavailable (`WhenExpression::unmet`).
struct UnmetCondition {
    enum class Kind : std::uint8_t {
        key,         ///< `key` alone is false.
        not_key,     ///< `!key` is false: the key is truthy.
        equals,      ///< `key == literal` (or `!(key != literal)`) is false.
        not_equals,  ///< `key != literal` (or `!(key == literal)`) is false.
        other,       ///< Anything else, as a whole: `a || b`, `x in list`, `!(a && b)`, `false`.
    };

    Kind kind = Kind::other;
    /// The key of `key`, `not_key`, `equals` and `not_equals`; empty for `other`.
    std::string key{};
    /// The value compared with, for `equals` and `not_equals`.
    ContextValue literal{};
    /// The condition in normal form (`WhenExpression::to_string`).
    std::string text{};

    friend bool operator==(const UnmetCondition&, const UnmetCondition&) = default;
};

/// A condition on context keys, written as a `when` clause of VS Code (docs/architecture.md §4.3):
///
///     expression := or
///     or         := and ('||' and)*
///     and        := unary ('&&' unary)*
///     unary      := '!' negated | primary
///     negated    := '!' negated | 'true' | 'false' | '(' expression ')' | key
///     primary    := 'true' | 'false' | '(' expression ')'
///                 | key [ ('==' | '!=') literal | 'in' key ]
///     literal    := text | number | 'true' | 'false'
///
/// Text is in single quotes and has no escapes; a number is -?digits(.digits)?([eE][+-]?digits)?.
/// '&&' binds tighter than '||'; '!' negates a key, true, false, '(...)' or another '!', so a
/// negated comparison is written !(stage == 'mesh') or stage != 'mesh'. Unlike VS Code, a word
/// without quotes after '==' is not text but an error.
///
/// A key alone is true if its value is (`ContextValue::is_truthy`). `key == literal` is true if
/// the value is of the same kind and equal (a key without a value equals nothing); `!=` is its
/// negation. `key in list` is true if the value of `list` is a list that contains the text in
/// `key`.
///
/// Expressions are immutable; copies share the parsed tree.
class WhenExpression {
public:
    /// Gives the value of a key; a key without a value gives a none value. The references must
    /// stay valid while the expression is evaluated.
    using Lookup = std::function<const ContextValue&(std::string_view key)>;

    /// Parses `text`. Throws WhenSyntaxError with the column and the reason.
    [[nodiscard]] static WhenExpression parse(std::string_view text);

    [[nodiscard]] bool evaluate(const Lookup& lookup) const;
    /// Why the expression is false: of an `&&` chain, the operands that are false (taken apart in
    /// turn); anything else as one condition, since any one operand of `||` would do. Empty if the
    /// expression is true.
    [[nodiscard]] std::vector<UnmetCondition> unmet(const Lookup& lookup) const;
    /// Keys the expression reads, sorted, each once.
    [[nodiscard]] const std::vector<std::string>& keys() const noexcept { return keys_; }
    /// Normal form: one space around binary operators, parentheses only where needed, literals as
    /// written. Parsing it gives the same normal form.
    [[nodiscard]] std::string to_string() const;

private:
    explicit WhenExpression(std::shared_ptr<const detail::WhenNode> root);

    std::shared_ptr<const detail::WhenNode> root_;
    std::vector<std::string> keys_;
};

}  // namespace fem::context
