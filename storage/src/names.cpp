#include "names.h"

#include <algorithm>
#include <array>
#include <cctype>

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

}  // namespace fem::storage::detail
