#pragma once

#include <optional>
#include <string_view>

namespace fem::detail {

/// Reads a decimal floating-point number that takes all of `text`, correctly rounded and whatever
/// the C locale is. Returns nullopt if `text` is not such a number; the caller checks the form
/// it accepts first, since infinities and NaN are read too.
[[nodiscard]] std::optional<double> parse_double(std::string_view text);

}  // namespace fem::detail
