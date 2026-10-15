#include "fem/core/command/python_literal.h"

#include "fem/core/command/error.h"
#include "fem/core/text/number.h"

#include "../model/utf8.h"
#include "../text/number.h"
#include "python_reader.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace fem::command {

namespace {

using fem::detail::parse_double;
using model::detail::is_valid_utf8;

[[noreturn]] void throw_invalid_value(const std::string& message) {
    throw CommandError(CommandErrc::invalid_value, message);
}

void append_hex(std::string& out, std::uint32_t value, int digits) {
    constexpr const char* hex = "0123456789abcdef";
    for (int shift = 4 * (digits - 1); shift >= 0; shift -= 4) {
        out.push_back(hex[(value >> static_cast<unsigned>(shift)) & 0xFU]);
    }
}

void append_string(std::string& out, std::string_view text) {
    if (!is_valid_utf8(text)) {
        throw_invalid_value("text is not valid UTF-8");
    }
    out.push_back('"');
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        const auto next = [&](std::size_t k) {
            return i + k < text.size() ? static_cast<unsigned char>(text[i + k]) : 0U;
        };
        switch (byte) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20U || byte == 0x7FU) {
                    out += "\\x";
                    append_hex(out, byte, 2);
                } else if (byte == 0xC2U && next(1) >= 0x80U && next(1) <= 0x9FU) {
                    // C1 control characters U+0080..U+009F.
                    out += "\\x";
                    append_hex(out, next(1), 2);
                    i += 1;
                } else if (byte == 0xE2U && next(1) == 0x80U &&
                           (next(2) == 0xA8U || next(2) == 0xA9U)) {
                    // U+2028 LINE SEPARATOR, U+2029 PARAGRAPH SEPARATOR.
                    out += "\\u";
                    append_hex(out, 0x2000U + (next(2) - 0x80U), 4);
                    i += 2;
                } else {
                    out.push_back(static_cast<char>(byte));
                }
        }
    }
    out.push_back('"');
}

// Python's repr() of a float (fem::text::format_double).
void append_number(std::string& out, double value) {
    if (!std::isfinite(value)) {
        throw_invalid_value("numbers in the journal must be finite");
    }
    out += text::format_double(value);
}

void append_value(std::string& out, const Value& value) {
    switch (value.kind()) {
        case Value::Kind::none:
            out += "None";
            break;
        case Value::Kind::boolean:
            out += value.as_boolean() ? "True" : "False";
            break;
        case Value::Kind::integer:
            out += std::to_string(value.as_integer());
            break;
        case Value::Kind::number:
            append_number(out, value.as_number());
            break;
        case Value::Kind::string:
            append_string(out, value.as_string());
            break;
        case Value::Kind::path: {
            const std::u8string text = value.as_path().generic_u8string();
            out += "Path(";
            append_string(
                out, std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
            out.push_back(')');
            break;
        }
        case Value::Kind::list: {
            out.push_back('[');
            bool first = true;
            for (const Value& item : value.as_list()) {
                if (!first) {
                    out += ", ";
                }
                first = false;
                append_value(out, item);
            }
            out.push_back(']');
            break;
        }
        case Value::Kind::dict: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, item] : value.as_dict()) {
                if (!first) {
                    out += ", ";
                }
                first = false;
                append_string(out, key);
                out += ": ";
                append_value(out, item);
            }
            out.push_back('}');
            break;
        }
    }
}

void append_utf8(std::string& out, char32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

}  // namespace

std::string to_python(const Value& value) {
    std::string out;
    append_value(out, value);
    return out;
}

std::string python_string(std::string_view text) {
    std::string out;
    append_string(out, text);
    return out;
}

Value parse_python(std::string_view text) {
    detail::PythonReader reader(text);
    Value value = reader.value();
    if (!reader.at_end()) {
        reader.fail("unexpected text after the value");
    }
    return value;
}

namespace detail {

void PythonReader::skip_spaces() noexcept {
    while (pos_ < text_.size() && text_[pos_] == ' ') {
        ++pos_;
    }
}

bool PythonReader::at_end() noexcept {
    skip_spaces();
    return pos_ == text_.size() || text_[pos_] == '#';
}

bool PythonReader::at_string() noexcept {
    skip_spaces();
    return pos_ < text_.size() && (text_[pos_] == '"' || text_[pos_] == '\'');
}

bool PythonReader::accept(std::string_view token) noexcept {
    skip_spaces();
    if (text_.substr(pos_, token.size()) != token) {
        return false;
    }
    pos_ += token.size();
    return true;
}

void PythonReader::expect(std::string_view token) {
    if (!accept(token)) {
        fail("expected '" + std::string(token) + "'");
    }
}

std::string_view PythonReader::name() noexcept {
    skip_spaces();
    const std::size_t start = pos_;
    if (pos_ < text_.size() && is_name_start(text_[pos_])) {
        ++pos_;
        while (pos_ < text_.size() && (is_name_start(text_[pos_]) || is_digit(text_[pos_]))) {
            ++pos_;
        }
    }
    return text_.substr(start, pos_ - start);
}

void PythonReader::fail(std::string_view message) const {
    std::size_t column = 1;
    for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
        if ((static_cast<unsigned char>(text_[i]) & 0xC0U) != 0x80U) {
            ++column;
        }
    }
    std::string where = line_ > 0 ? "line " + std::to_string(line_) + ", " : std::string();
    where += "column " + std::to_string(column) + ": ";
    throw CommandError(CommandErrc::journal_syntax, where + std::string(message));
}

std::string PythonReader::string_literal() {
    if (!at_string()) {
        fail("expected text in quotes");
    }
    const char quote = text_[pos_++];
    std::string out;
    while (true) {
        if (pos_ >= text_.size()) {
            fail("the text is not closed");
        }
        const char c = text_[pos_++];
        if (c == quote) {
            break;
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (pos_ >= text_.size()) {
            fail("the text is not closed");
        }
        const char escape = text_[pos_++];
        switch (escape) {
            case '\\':
            case '\'':
            case '"':
                out.push_back(escape);
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'a':
                out.push_back('\a');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'v':
                out.push_back('\v');
                break;
            case 'x':
                append_utf8(out, hex_escape(2));
                break;
            case 'u':
                append_utf8(out, hex_escape(4));
                break;
            case 'U':
                append_utf8(out, hex_escape(8));
                break;
            default:
                --pos_;
                fail("unsupported escape sequence");
        }
    }
    if (!is_valid_utf8(out)) {
        fail("the text is not valid UTF-8");
    }
    return out;
}

char32_t PythonReader::hex_escape(int digits) {
    std::uint32_t code_point = 0;
    for (int i = 0; i < digits; ++i) {
        if (pos_ >= text_.size()) {
            fail("incomplete escape sequence");
        }
        const char c = text_[pos_];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            fail("incomplete escape sequence");
        }
        code_point = code_point * 16 + digit;
        ++pos_;
    }
    if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
        fail("the escape sequence is not a Unicode character");
    }
    return static_cast<char32_t>(code_point);
}

Value PythonReader::number() {
    const std::size_t start = pos_;
    if (text_[pos_] == '-') {
        ++pos_;
    }
    const std::size_t digits_start = pos_;
    bool fractional = false;
    while (pos_ < text_.size()) {
        const char c = text_[pos_];
        if (is_digit(c)) {
            ++pos_;
        } else if (c == '.') {
            fractional = true;
            ++pos_;
        } else if (c == 'e' || c == 'E') {
            fractional = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
        } else {
            break;
        }
    }
    const std::string_view token = text_.substr(start, pos_ - start);
    const std::string_view digits = text_.substr(digits_start, pos_ - digits_start);
    if (digits.empty()) {
        pos_ = start;
        fail("expected a number");
    }
    if (!fractional) {
        if (digits.size() > 1 && digits[0] == '0') {
            pos_ = start;
            fail("leading zeros are not allowed in integers");
        }
        std::int64_t value = 0;
        const std::from_chars_result result =
            std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            pos_ = start;
            fail("the integer does not fit into 64 bits");
        }
        return Value(value);
    }
    const std::optional<double> value = parse_double(token);
    if (!value || !std::isfinite(*value)) {
        pos_ = start;
        fail("not a finite number");
    }
    return Value(*value);
}

Value PythonReader::value() {
    skip_spaces();
    if (pos_ >= text_.size()) {
        fail("expected a value");
    }
    const char c = text_[pos_];
    if (c == '"' || c == '\'') {
        return Value(string_literal());
    }
    if (c == '-' || c == '.' || is_digit(c)) {
        return number();
    }
    if (c == '[') {
        ++pos_;
        Value::List list;
        while (!accept("]")) {
            list.push_back(value());
            if (accept("]")) {
                break;
            }
            expect(",");
        }
        return Value(std::move(list));
    }
    if (c == '{') {
        ++pos_;
        Value::Dict dict;
        while (!accept("}")) {
            if (!at_string()) {
                fail("dictionary keys must be text");
            }
            std::string key = string_literal();
            expect(":");
            dict.insert_or_assign(std::move(key), value());
            if (accept("}")) {
                break;
            }
            expect(",");
        }
        return Value(std::move(dict));
    }

    const std::size_t start = pos_;
    const std::string_view word = name();
    if (word == "None") {
        return Value();
    }
    if (word == "True") {
        return Value(true);
    }
    if (word == "False") {
        return Value(false);
    }
    if (word == "Path") {
        expect("(");
        const std::string text = string_literal();
        accept(",");
        expect(")");
        return Value(std::filesystem::path(std::u8string(text.begin(), text.end())));
    }
    pos_ = start;
    fail("expected a value: None, True, False, a number, text, Path(...), a list or a dictionary");
}

}  // namespace detail

}  // namespace fem::command
