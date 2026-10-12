#pragma once

#include "fem/core/model/object_id.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace fem::command {

/// Argument or result of a command: what a Python call can pass and the journal can write back
/// (docs/architecture.md §4.6, item 5). Text is UTF-8; paths are kept as paths, so the journal
/// writes them as `Path("...")`; dictionaries have text keys and are ordered by them.
class Value {
public:
    using List = std::vector<Value>;
    using Dict = std::map<std::string, Value, std::less<>>;

    enum class Kind : std::uint8_t { none, boolean, integer, number, string, path, list, dict };

    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    Value(bool value) noexcept : data_(value) {}
    /// Any integer type except bool and characters; throws CommandError(invalid_value) for
    /// unsigned values above the int64 range.
    template <std::integral T>
        requires(!std::same_as<T, bool> && !std::same_as<T, char> && !std::same_as<T, char8_t> &&
                 !std::same_as<T, char16_t> && !std::same_as<T, char32_t> &&
                 !std::same_as<T, wchar_t>)
    Value(T value) : data_(to_int64(value)) {}
    Value(double value) noexcept : data_(value) {}
    Value(std::string value) noexcept : data_(std::move(value)) {}
    Value(std::string_view value) : data_(std::string(value)) {}
    Value(const char* value) : data_(std::string(value)) {}
    Value(std::filesystem::path value) noexcept : data_(std::move(value)) {}
    /// An object id is written as its number.
    Value(model::ObjectId id) : Value(id.value()) {}
    Value(List value) noexcept : data_(std::move(value)) {}
    Value(Dict value) noexcept : data_(std::move(value)) {}

    [[nodiscard]] Kind kind() const noexcept { return static_cast<Kind>(data_.index()); }
    [[nodiscard]] bool is_none() const noexcept { return kind() == Kind::none; }

    // Accessors throw CommandError(invalid_value) if the value is of another kind.
    [[nodiscard]] bool as_boolean() const;
    [[nodiscard]] std::int64_t as_integer() const;
    /// A number or an integer, as double.
    [[nodiscard]] double as_number() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const std::filesystem::path& as_path() const;
    [[nodiscard]] const List& as_list() const;
    [[nodiscard]] List& as_list();
    [[nodiscard]] const Dict& as_dict() const;
    [[nodiscard]] Dict& as_dict();

    /// Same kind and equal contents. An integer never equals a number; paths compare as paths.
    friend bool operator==(const Value& a, const Value& b);

private:
    template <class T>
    static std::int64_t to_int64(T value) {
        if constexpr (std::is_unsigned_v<T> && sizeof(T) >= sizeof(std::int64_t)) {
            if (value > static_cast<T>(std::numeric_limits<std::int64_t>::max())) {
                throw_out_of_range();
            }
        }
        return static_cast<std::int64_t>(value);
    }
    [[noreturn]] static void throw_out_of_range();
    [[noreturn]] void throw_kind(Kind expected) const;

    // The order of the alternatives matches `Kind`.
    std::variant<std::monostate, bool, std::int64_t, double, std::string, std::filesystem::path,
                 List, Dict>
        data_;
};

/// "none", "boolean", "integer", "number", "string", "path", "list", "dict".
[[nodiscard]] std::string_view to_string(Value::Kind kind) noexcept;

}  // namespace fem::command
