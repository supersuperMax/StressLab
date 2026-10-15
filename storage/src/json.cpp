#include "fem/storage/json.h"

#include "fem/core/text/number.h"
#include "fem/storage/error.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fem::storage {

namespace {

[[noreturn]] void throw_invalid(const std::string& message) {
    throw ProjectError(ProjectErrc::invalid_data, message);
}

void append_string(std::string& out, const std::string& text) {
    try {
        // nlohmann escapes exactly `"`, `\` and control characters and checks the UTF-8.
        out += nlohmann::json(text).dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
    } catch (const nlohmann::json::type_error&) {
        throw_invalid("text is not valid UTF-8");
    }
}

void append_indent(std::string& out, int level) {
    out.append(static_cast<std::size_t>(level) * 2, ' ');
}

void append_value(std::string& out, const nlohmann::json& value, int level) {
    using Type = nlohmann::json::value_t;
    switch (value.type()) {
        case Type::null:
            out += "null";
            return;
        case Type::boolean:
            out += value.get<bool>() ? "true" : "false";
            return;
        case Type::number_integer:
            out += std::to_string(value.get<std::int64_t>());
            return;
        case Type::number_unsigned:
            out += std::to_string(value.get<std::uint64_t>());
            return;
        case Type::number_float: {
            const double number = value.get<double>();
            if (!std::isfinite(number)) {
                throw_invalid("numbers must be finite");
            }
            out += text::format_double(number);
            return;
        }
        case Type::string:
            append_string(out, value.get_ref<const std::string&>());
            return;
        case Type::array: {
            if (value.empty()) {
                out += "[]";
                return;
            }
            out += "[\n";
            bool first = true;
            for (const nlohmann::json& item : value) {
                out += first ? "" : ",\n";
                first = false;
                append_indent(out, level + 1);
                append_value(out, item, level + 1);
            }
            out += '\n';
            append_indent(out, level);
            out += ']';
            return;
        }
        case Type::object: {
            if (value.empty()) {
                out += "{}";
                return;
            }
            // nlohmann keeps members in a std::map: sorted by the bytes of the keys.
            out += "{\n";
            bool first = true;
            for (auto it = value.begin(); it != value.end(); ++it) {
                out += first ? "" : ",\n";
                first = false;
                append_indent(out, level + 1);
                append_string(out, it.key());
                out += ": ";
                append_value(out, it.value(), level + 1);
            }
            out += '\n';
            append_indent(out, level);
            out += '}';
            return;
        }
        case Type::binary:
        case Type::discarded:
            break;
    }
    throw_invalid("binary values cannot be written to JSON");
}

int line_of(std::string_view text, std::size_t byte) {
    byte = std::min(byte, text.size());
    return 1 + static_cast<int>(std::count(text.begin(), text.begin() + byte, '\n'));
}

}  // namespace

std::string to_canonical_json(const nlohmann::json& value) {
    std::string out;
    append_value(out, value, 0);
    out += '\n';
    return out;
}

nlohmann::json parse_json(std::string_view text, const std::string& file) {
    constexpr std::string_view bom = "\xEF\xBB\xBF";
    if (text.starts_with(bom)) {
        text.remove_prefix(bom.size());
    }
    try {
        return nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& error) {
        // `byte` is 1-based and points just past the offending character.
        const std::size_t byte = error.byte > 0 ? error.byte - 1 : 0;
        throw ProjectError(ProjectErrc::invalid_file, "invalid JSON", file, line_of(text, byte));
    }
}

}  // namespace fem::storage
