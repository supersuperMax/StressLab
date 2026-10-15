#pragma once

#include <string>

namespace fem::text {

/// A finite double as Python's repr() writes it: the shortest digits that read back to the same
/// double, positional for decimal exponents from -4 to 15 and exponential otherwise, always with a
/// point or an exponent ("1.0", "0.1", "1e+100", "5e-324", "-0.0"). The journal (.slpy) and the
/// project files (.slproj) write numbers this way, so a number reads the same in both and a float
/// never turns into an integer. Throws std::domain_error for NaN and infinities.
[[nodiscard]] std::string format_double(double value);

}  // namespace fem::text
