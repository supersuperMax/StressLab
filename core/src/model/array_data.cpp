#include "fem/core/model/array_data.h"

#include "fem/core/model/error.h"

#include <algorithm>
#include <array>
#include <string>

namespace fem::model {

namespace {

constexpr std::size_t max_name_size = 64;

[[noreturn]] void throw_invalid(const std::string& message) {
    throw ModelError(ModelErrc::invalid_data, message);
}

bool is_name_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

constexpr std::array<std::string_view, 3> type_names{"int32", "int64", "float64"};

}  // namespace

std::string_view to_string(ScalarType type) noexcept {
    return type_names[static_cast<std::size_t>(type)];
}

std::optional<ScalarType> parse_scalar_type(std::string_view text) noexcept {
    for (std::size_t i = 0; i < type_names.size(); ++i) {
        if (type_names[i] == text) {
            return static_cast<ScalarType>(i);
        }
    }
    return std::nullopt;
}

NamedArray::NamedArray(std::string name, Items items, std::size_t width)
    : name_(std::move(name)), items_(std::move(items)), width_(width) {
    if (name_.empty() || name_.size() > max_name_size ||
        !std::all_of(name_.begin(), name_.end(), is_name_char)) {
        throw_invalid("the name of an array must be 1 to 64 ASCII letters, digits and '_': '" +
                      name_ + "'");
    }
    if (width_ == 0) {
        throw_invalid("the array '" + name_ + "' needs a width of at least 1");
    }
    if (size() % width_ != 0) {
        throw_invalid("the array '" + name_ + "' has " + std::to_string(size()) +
                      " items, not rows of " + std::to_string(width_));
    }
}

std::size_t NamedArray::size() const noexcept {
    return std::visit([](const auto& values) { return values.size(); }, items_);
}

void NamedArray::throw_wrong_type() const {
    throw_invalid("the array '" + name_ + "' holds " + std::string(to_string(type())) + " items");
}

ArrayData::ArrayData(std::vector<NamedArray> arrays) : arrays_(std::move(arrays)) {
    std::sort(arrays_.begin(), arrays_.end(),
              [](const NamedArray& a, const NamedArray& b) { return a.name() < b.name(); });
    for (std::size_t i = 0; i < arrays_.size(); ++i) {
        if (i > 0 && arrays_[i].name() == arrays_[i - 1].name()) {
            throw_invalid("two arrays are named '" + arrays_[i].name() + "'");
        }
        if (arrays_[i].rows() != arrays_.front().rows()) {
            throw_invalid("the array '" + arrays_[i].name() + "' has " +
                          std::to_string(arrays_[i].rows()) + " rows and the array '" +
                          arrays_.front().name() + "' " + std::to_string(arrays_.front().rows()));
        }
    }
    rows_ = arrays_.empty() ? 0 : arrays_.front().rows();
}

const NamedArray* ArrayData::find(std::string_view name) const noexcept {
    const auto found = std::lower_bound(
        arrays_.begin(), arrays_.end(), name,
        [](const NamedArray& array, std::string_view key) { return array.name() < key; });
    return found != arrays_.end() && found->name() == name ? &*found : nullptr;
}

}  // namespace fem::model
