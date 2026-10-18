#pragma once

#include "fem/core/model/object_data.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fem::model {

/// Type of the items of a `NamedArray`.
enum class ScalarType : std::uint8_t { int32, int64, float64 };

/// "int32", "int64", "float64".
[[nodiscard]] std::string_view to_string(ScalarType type) noexcept;
[[nodiscard]] std::optional<ScalarType> parse_scalar_type(std::string_view text) noexcept;

/// One array of an `ArrayData`: rows of `width` items each, row after row.
///
/// Move-only, like `Mesh`, so that the items are never copied by accident.
class NamedArray {
public:
    using Items =
        std::variant<std::vector<std::int32_t>, std::vector<std::int64_t>, std::vector<double>>;

    /// Takes ownership of the items without copying them. Throws ModelError(invalid_data) if the
    /// name is not 1 to 64 ASCII letters, digits and '_', the width is 0, or the item count is not
    /// a multiple of the width.
    NamedArray(std::string name, Items items, std::size_t width = 1);

    NamedArray(NamedArray&&) noexcept = default;
    NamedArray& operator=(NamedArray&&) noexcept = default;
    NamedArray(const NamedArray&) = delete;
    NamedArray& operator=(const NamedArray&) = delete;
    ~NamedArray() = default;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] ScalarType type() const noexcept {
        return static_cast<ScalarType>(items_.index());
    }
    /// Items per row.
    [[nodiscard]] std::size_t width() const noexcept { return width_; }
    [[nodiscard]] std::size_t rows() const noexcept { return size() / width_; }
    /// Items in all rows.
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const Items& items() const noexcept { return items_; }

    /// The items as `T` (std::int32_t, std::int64_t or double). Throws ModelError(invalid_data)
    /// if the array holds items of another type.
    template <class T>
    [[nodiscard]] std::span<const T> as() const {
        if (const auto* values = std::get_if<std::vector<T>>(&items_)) {
            return *values;
        }
        throw_wrong_type();
    }

private:
    [[noreturn]] void throw_wrong_type() const;

    std::string name_;
    Items items_;
    std::size_t width_;
};

/// Bulk data of an object of a module (docs/adr/0023-structural-model-and-results-ir.md): named
/// arrays with the same number of rows, such as the values of a field of results and the nodes they
/// belong to. The core does not know what the arrays mean; the module that owns the type does.
/// Immutable, like every `ObjectData`. Items may be any value, also NaN: they are stored as they
/// are, bit for bit.
class ArrayData final : public ObjectData {
public:
    /// Takes ownership of the arrays without copying their items and orders them by name. Throws
    /// ModelError(invalid_data) if two arrays have the same name or a different number of rows.
    explicit ArrayData(std::vector<NamedArray> arrays);

    ArrayData(ArrayData&&) noexcept = default;
    ArrayData& operator=(ArrayData&&) noexcept = default;
    ArrayData(const ArrayData&) = delete;
    ArrayData& operator=(const ArrayData&) = delete;
    ~ArrayData() override = default;

    /// Rows of every array; 0 without arrays.
    [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
    /// By name.
    [[nodiscard]] std::span<const NamedArray> arrays() const noexcept { return arrays_; }
    /// The array with this name, or nullptr.
    [[nodiscard]] const NamedArray* find(std::string_view name) const noexcept;

private:
    std::vector<NamedArray> arrays_;
    std::size_t rows_ = 0;
};

}  // namespace fem::model
