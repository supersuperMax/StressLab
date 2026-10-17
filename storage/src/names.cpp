#include "names.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>

namespace fem::storage::detail {

namespace {

bool is_plain(char c, bool for_path) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || (for_path && c == '.') ||
           // In references, text beyond ASCII stays as it is.
           (!for_path && static_cast<unsigned char>(c) >= 0x80);
}

void append_escaped(std::string& out, char c) {
    constexpr const char* hex = "0123456789ABCDEF";
    const auto byte = static_cast<unsigned char>(c);
    out.push_back('%');
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0xF]);
}

// Names Windows keeps for devices, with or without an extension.
bool is_reserved_name(std::string_view segment) {
    static constexpr std::array<std::string_view, 22> reserved = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    std::string stem(segment.substr(0, segment.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return std::find(reserved.begin(), reserved.end(), stem) != reserved.end();
}

std::string escaped(std::string_view name, bool for_path) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        // A leading or trailing dot ("..", ".git", "a.") and the first letter of a device name are
        // escaped as well, so the segment is an ordinary name everywhere.
        const bool edge_dot = c == '.' && (i == 0 || i + 1 == name.size());
        // A reference must not look like an option of git ("-f").
        const bool leading_dash = !for_path && i == 0 && c == '-';
        if (!is_plain(c, for_path) || edge_dot || leading_dash ||
            (i == 0 && is_reserved_name(name))) {
            append_escaped(out, c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

}  // namespace

std::string path_segment(std::string_view name) {
    return escaped(name, true);
}

std::string ref_segment(std::string_view name) {
    return escaped(name, false);
}

std::optional<std::string> unescape_segment(std::string_view segment) {
    std::string out;
    for (std::size_t i = 0; i < segment.size(); ++i) {
        if (segment[i] != '%') {
            out.push_back(segment[i]);
            continue;
        }
        if (i + 2 >= segment.size()) {
            return std::nullopt;
        }
        const int high = hex_value(segment[i + 1]);
        const int low = hex_value(segment[i + 2]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return out;
}

bool is_utf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        std::uint32_t code = byte;
        if (byte >= 0xF0 && byte <= 0xF4) {
            length = 4;
            code = byte & 0x07U;
        } else if (byte >= 0xE0) {
            length = byte <= 0xEF ? 3 : 0;
            code = byte & 0x0FU;
        } else if (byte >= 0xC2) {
            length = 2;
            code = byte & 0x1FU;
        } else if (byte >= 0x80) {
            length = 0;
        }
        if (length == 0 || i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        if ((length == 3 && (code < 0x800 || (code >= 0xD800 && code <= 0xDFFF))) ||
            (length == 4 && (code < 0x10000 || code > 0x10FFFF))) {
            return false;
        }
        i += length;
    }
    return true;
}

bool is_portable_relative_path(std::string_view path) noexcept {
    if (path.empty() || !is_utf8(path)) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' ||
            segment.back() == ' ' || is_reserved_name(segment)) {
            return false;
        }
        for (const char c : segment) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F || c == ':' || c == '\\' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') {
                return false;
            }
        }
        start = end + 1;
    }
    return true;
}

std::string fold_case(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        const std::size_t length = byte < 0x80 ? 1 : byte < 0xE0 ? 2 : byte < 0xF0 ? 3 : 4;
        if (length == 1) {
            out.push_back(byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a')
                                                     : static_cast<char>(byte));
        } else if (length == 2 && i + 1 < text.size()) {
            std::uint32_t code =
                ((byte & 0x1FU) << 6U) | (static_cast<unsigned char>(text[i + 1]) & 0x3FU);
            if (code >= 0x410 && code <= 0x42F) {
                code += 0x20;  // А-Я
            } else if (code >= 0x400 && code <= 0x40F) {
                code += 0x50;  // Ѐ-Џ, Ё among them
            } else if (code >= 0xC0 && code <= 0xDE && code != 0xD7) {
                code += 0x20;  // Latin-1 capitals
            }
            out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else {
            out.append(text.substr(i, length));
        }
        i += length;
    }
    return out;
}

}  // namespace fem::storage::detail
