#include "inp/encoding.h"

#include <array>
#include <cstdint>

namespace fem::inp {

namespace {

char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ascii_lower(std::string_view text, std::string_view lower) noexcept {
    if (text.size() != lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (ascii_lower(text[i]) != lower[i]) {
            return false;
        }
    }
    return true;
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

// Code points of the bytes 0x80..0xBF; 0xC0..0xFF follow a rule. Bytes the code page leaves
// undefined map to the C1 control of the same value, as the WHATWG encoding standard does.
constexpr std::array<std::uint16_t, 64> windows1251_high{
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,  // 80
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,  // 88
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  // 90
    0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,  // 98
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,  // A0
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,  // A8
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,  // B0
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,  // B8
};

constexpr std::array<std::uint16_t, 32> windows1252_high{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,  // 80
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,  // 88
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  // 90
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,  // 98
};

std::uint32_t code_point(Encoding encoding, unsigned char byte) noexcept {
    if (byte < 0x80) {
        return byte;
    }
    switch (encoding) {
        case Encoding::windows1251:
            return byte >= 0xC0 ? 0x0410U + (byte - 0xC0U) : windows1251_high[byte - 0x80U];
        case Encoding::windows1252:
            return byte >= 0xA0 ? byte : windows1252_high[byte - 0x80U];
        default:
            return byte;  // Latin-1: the byte is the code point.
    }
}

std::string decode_single_byte(std::string_view bytes, Encoding encoding) {
    std::string out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        append_utf8(out, code_point(encoding, static_cast<unsigned char>(c)));
    }
    return out;
}

std::optional<std::string> decode_utf16(std::string_view bytes, bool big_endian) {
    if (bytes.size() % 2 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(bytes.size());
    const auto unit = [&](std::size_t i) -> std::uint32_t {
        const auto first = static_cast<unsigned char>(bytes[i]);
        const auto second = static_cast<unsigned char>(bytes[i + 1]);
        return big_endian ? (first << 8U) | second : (second << 8U) | first;
    };
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        std::uint32_t code = unit(i);
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (i + 3 >= bytes.size()) {
                return std::nullopt;
            }
            const std::uint32_t low = unit(i + 2);
            if (low < 0xDC00 || low > 0xDFFF) {
                return std::nullopt;
            }
            code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
            i += 2;
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return std::nullopt;
        }
        append_utf8(out, code);
    }
    return out;
}

}  // namespace

std::optional<Encoding> parse_encoding(std::string_view name) noexcept {
    for (const Encoding encoding : {Encoding::automatic, Encoding::utf8, Encoding::windows1251,
                                    Encoding::windows1252, Encoding::latin1}) {
        if (equals_ascii_lower(name, to_string(encoding))) {
            return encoding;
        }
    }
    return std::nullopt;
}

std::string_view to_string(Encoding encoding) noexcept {
    switch (encoding) {
        case Encoding::automatic:
            return "auto";
        case Encoding::utf8:
            return "utf-8";
        case Encoding::windows1251:
            return "windows-1251";
        case Encoding::windows1252:
            return "windows-1252";
        case Encoding::latin1:
            return "latin-1";
    }
    return "auto";
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            length = 2;
            code = c & 0x1FU;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
            code = c & 0x0FU;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
            code = c & 0x07U;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        constexpr std::array<std::uint32_t, 5> smallest{0, 0, 0x80, 0x800, 0x10000};
        if (code < smallest[length] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

std::optional<DecodedText> decode(std::string_view bytes, Encoding requested) {
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.remove_prefix(3);
        if (!is_valid_utf8(bytes)) {
            return std::nullopt;
        }
        return DecodedText{std::string(bytes), "utf-8", false};
    }
    if (bytes.starts_with("\xFF\xFE") || bytes.starts_with("\xFE\xFF")) {
        const bool big_endian = bytes[0] == '\xFE';
        std::optional<std::string> text = decode_utf16(bytes.substr(2), big_endian);
        if (!text) {
            return std::nullopt;
        }
        return DecodedText{std::move(*text), big_endian ? "utf-16be" : "utf-16le", false};
    }
    switch (requested) {
        case Encoding::automatic:
            if (is_valid_utf8(bytes)) {
                return DecodedText{std::string(bytes), "utf-8", false};
            }
            return DecodedText{decode_single_byte(bytes, Encoding::windows1252),
                               std::string(to_string(Encoding::windows1252)), true};
        case Encoding::utf8:
            if (!is_valid_utf8(bytes)) {
                return std::nullopt;
            }
            return DecodedText{std::string(bytes), "utf-8", false};
        default:
            return DecodedText{decode_single_byte(bytes, requested),
                               std::string(to_string(requested)), false};
    }
}

}  // namespace fem::inp
