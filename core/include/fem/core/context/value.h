#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fem::context {

/// Value of a context key: nothing, a boolean, a number, UTF-8 text or a list of texts (for
/// `in`). A key that is not declared or not set has no value.
class ContextValue {
public:
    using List = std::vector<std::string>;

    enum class Kind : std::uint8_t { none, boolean, number, string, list };

    ContextValue() noexcept = default;
    ContextValue(std::nullptr_t) noexcept {}
    ContextValue(bool value) noexcept : data_(value) {}
    /// Integers become numbers; above 2^53 they are rounded.
    template <std::integral T>
        requires(!std::same_as<T, bool> && !std::same_as<T, char> && !std::same_as<T, char8_t> &&
                 !std::same_as<T, char16_t> && !std::same_as<T, char32_t> &&
                 !std::same_as<T, wchar_t>)
    ContextValue(T value) noexcept : data_(static_cast<double>(value)) {}
    ContextValue(double value) noexcept : data_(value) {}
    ContextValue(std::string value) noexcept : data_(std::move(value)) {}
    ContextValue(std::string_view value) : data_(std::string(value)) {}
    /// Text, not a boolean.
    ContextValue(const char* value) : data_(std::string(value)) {}
    ContextValue(List value) noexcept : data_(std::move(value)) {}

    [[nodiscard]] Kind kind() const noexcept { return static_cast<Kind>(data_.index()); }
    [[nodiscard]] bool is_none() const noexcept { return kind() == Kind::none; }

    // Accessors throw ContextError(invalid_value) if the value is of another kind.
    [[nodiscard]] bool as_boolean() const;
    [[nodiscard]] double as_number() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const List& as_list() const;

    /// The value of a key standing alone in a `when` expression: false for no value, false, 0,
    /// empty text and an empty list; true otherwise.
    [[nodiscard]] bool is_truthy() const noexcept;

    /// Same kind and equal contents; this is `==` of the `when` language.
    friend bool operator==(const ContextValue&, const ContextValue&) = default;

private:
    [[noreturn]] void throw_kind(Kind expected) const;

    // The order of the alternatives matches `Kind`.
    std::variant<std::monostate, bool, double, std::string, List> data_;
};

/// "none", "boolean", "number", "string", "list".
[[nodiscard]] std::string_view to_string(ContextValue::Kind kind) noexcept;

}  // namespace fem::context
