#include "inp/numbers.h"

#include <array>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <system_error>

namespace fem::inp {

namespace {

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

}  // namespace

std::optional<double> parse_number(std::string_view text) noexcept {
    // Checks the grammar first, so that the conversion below sees only a plain number:
    // [+-] digits [. digits] [(e|E|d|D) [+-] digits], with at least one digit in the mantissa.
    constexpr std::size_t max_length = 64;
    if (text.empty() || text.size() > max_length) {
        return std::nullopt;
    }
    std::array<char, max_length + 1> buffer{};
    std::size_t i = 0;
    std::size_t out = 0;
    std::size_t point = max_length + 1;  // Position of the point in `buffer`, if any.
    if (text[i] == '+' || text[i] == '-') {
        buffer[out++] = text[i++];
    }
    bool digits = false;
    while (i < text.size() && is_digit(text[i])) {
        buffer[out++] = text[i++];
        digits = true;
    }
    if (i < text.size() && text[i] == '.') {
        point = out;
        buffer[out++] = text[i++];
        while (i < text.size() && is_digit(text[i])) {
            buffer[out++] = text[i++];
            digits = true;
        }
    }
    if (!digits) {
        return std::nullopt;
    }
    if (i < text.size()) {
        const char e = text[i];
        if (e != 'e' && e != 'E' && e != 'd' && e != 'D') {
            return std::nullopt;
        }
        buffer[out++] = 'e';
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            buffer[out++] = text[i++];
        }
        bool exponent = false;
        while (i < text.size() && is_digit(text[i])) {
            buffer[out++] = text[i++];
            exponent = true;
        }
        if (!exponent || i != text.size()) {
            return std::nullopt;
        }
    }
    double value = 0.0;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const char* first = buffer.data();
    if (*first == '+') {
        ++first;  // from_chars does not take a plus sign.
    }
    const std::from_chars_result result = std::from_chars(first, buffer.data() + out, value);
    if (result.ec != std::errc() || result.ptr != buffer.data() + out) {
        return std::nullopt;
    }
#else
    // libc++ of older macOS has no from_chars for double: strtod reads the decimal separator
    // of the C locale, so the point is replaced by it.
    if (point < out) {
        const char* separator = std::localeconv()->decimal_point;
        if (separator != nullptr && separator[0] != '\0' && separator[1] == '\0') {
            buffer[point] = separator[0];
        }
    }
    char* end = nullptr;
    value = std::strtod(buffer.data(), &end);
    if (end != buffer.data() + out) {
        return std::nullopt;
    }
#endif
    (void)point;
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> parse_integer(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    if (*first == '+') {
        ++first;
    }
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec == std::errc() && result.ptr == last) {
        return value;
    }
    const std::optional<double> number = parse_number(text);
    if (!number || std::trunc(*number) != *number ||
        std::abs(*number) > static_cast<double>(std::int64_t{1} << 53)) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*number);
}

}  // namespace fem::inp
