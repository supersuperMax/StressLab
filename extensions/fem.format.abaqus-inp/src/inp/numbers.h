#pragma once

// Numbers of data lines, whatever the locale of the process (Qt sets the C locale from the
// environment on Unix, where "1.5" would then read as 1): "12", "-3", "1.", ".5", "2.1e5",
// "2.1E+05", Fortran's "2.1D5".

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fem::inp {

/// A finite double; nothing for anything else, including text around the number.
[[nodiscard]] std::optional<double> parse_number(std::string_view text) noexcept;

/// An integer, also written as a number with a zero fraction ("5." or "5.0"); nothing otherwise.
[[nodiscard]] std::optional<std::int64_t> parse_integer(std::string_view text) noexcept;

/// The shortest text that `parse_number` reads back as `value`, whatever the locale; always with
/// a point or an exponent ("1.", "0.3", "2.1e+05"). `value` must be finite.
[[nodiscard]] std::string format_number(double value);

}  // namespace fem::inp
