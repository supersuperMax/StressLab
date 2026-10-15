#include "number.h"

#include "fem/core/text/number.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
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

namespace fem::text {

// Python/pystrtod.c, mode 'r': to_chars gives the shortest digits, the rest is their layout.
std::string format_double(double value) {
    if (!std::isfinite(value)) {
        throw std::domain_error("only finite numbers can be written");
    }
    char buffer[64];
    const std::to_chars_result result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
    std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
    std::string out;
    if (text.front() == '-') {
        out.push_back('-');
        text.remove_prefix(1);
    }
    // "d.ddde+XX" -> digits "dddd" and exponent XX.
    const std::size_t e = text.find('e');
    std::string digits(1, text[0]);
    if (e > 1) {
        digits.append(text.substr(2, e - 2));
    }
    int exponent = 0;
    const std::string_view exponent_text = text.substr(e + (text[e + 1] == '+' ? 2 : 1));
    const std::from_chars_result parsed = std::from_chars(
        exponent_text.data(), exponent_text.data() + exponent_text.size(), exponent);
    (void)parsed;  // to_chars wrote the exponent, so it parses.

    const int count = static_cast<int>(digits.size());
    const int point = exponent + 1;  // Position of the decimal point within the digits.
    if (point <= -4 || point > 16) {
        out.push_back(digits[0]);
        if (count > 1) {
            out.push_back('.');
            out.append(digits, 1);
        }
        out.push_back('e');
        out.push_back(exponent < 0 ? '-' : '+');
        const std::string magnitude = std::to_string(std::abs(exponent));
        if (magnitude.size() < 2) {
            out.push_back('0');
        }
        out += magnitude;
    } else if (point <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-point), '0');
        out += digits;
    } else if (point >= count) {
        out += digits;
        out.append(static_cast<std::size_t>(point - count), '0');
        out += ".0";
    } else {
        out.append(digits, 0, static_cast<std::size_t>(point));
        out.push_back('.');
        out.append(digits, static_cast<std::size_t>(point));
    }
    return out;
}

}  // namespace fem::text
