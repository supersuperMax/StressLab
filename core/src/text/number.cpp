#include "number.h"

#include <charconv>
#include <string>
#include <system_error>

#if !defined(__cpp_lib_to_chars)
#include <algorithm>
#include <clocale>
#include <cstdlib>
#endif

namespace fem::detail {

std::optional<double> parse_double(std::string_view text) {
    double value = 0.0;
#if defined(__cpp_lib_to_chars)
    const std::from_chars_result result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
#else
    // Without floating-point from_chars (libc++ on macOS): strtod, which is correctly rounded and
    // reads subnormal numbers (a stream refuses them). It follows the C locale, which a GUI may
    // set to one with ',', so the decimal point of that locale replaces '.' first.
    std::string copy(text);
    if (const char point = *std::localeconv()->decimal_point; point != '.') {
        std::replace(copy.begin(), copy.end(), '.', point);
    }
    char* end = nullptr;
    value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size()) {
        return std::nullopt;
    }
#endif
    return value;
}

}  // namespace fem::detail
