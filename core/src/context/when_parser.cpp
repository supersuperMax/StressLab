#include "fem/core/context/error.h"

#include "../model/utf8.h"
#include "../text/number.h"
#include "when_node.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fem::context::detail {

namespace {

using Op = WhenNode::Op;

enum class TokenKind : std::uint8_t {
    end,
    key,
    text,
    number,
    true_word,
    false_word,
    in_word,
    and_op,
    or_op,
    not_op,
    equals_op,
    not_equals_op,
    open,
    close,
};

struct Token {
    TokenKind kind = TokenKind::end;
    std::size_t offset = 0;
    std::string_view text{};  ///< As written; text keeps its quotes.
    double number = 0.0;
};

[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

[[nodiscard]] bool is_comparison(TokenKind kind) noexcept {
    return kind == TokenKind::equals_op || kind == TokenKind::not_equals_op ||
           kind == TokenKind::in_word;
}

/// -?digits(.digits)?([eE][+-]?digits)?
[[nodiscard]] bool is_number(std::string_view text) noexcept {
    std::size_t i = 0;
    const auto digits = [&] {
        const std::size_t from = i;
        while (i < text.size() && is_ascii_digit(text[i])) {
            ++i;
        }
        return i > from;
    };
    if (i < text.size() && text[i] == '-') {
        ++i;
    }
    if (!digits()) {
        return false;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        if (!digits()) {
            return false;
        }
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        if (!digits()) {
            return false;
        }
    }
    return i == text.size();
}

[[nodiscard]] std::string hex(std::uint32_t value, int digits) {
    constexpr std::string_view hex_digits = "0123456789ABCDEF";
    std::string out(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = hex_digits[value & 0xFU];
        value >>= 4U;
    }
    return out;
}

[[nodiscard]] std::string describe(const Token& token) {
    switch (token.kind) {
        case TokenKind::end:
            return "the end of the expression";
        case TokenKind::key:
            return "key '" + std::string(token.text) + "'";
        case TokenKind::text:
            return "text " + std::string(token.text);
        case TokenKind::number:
            return "number " + std::string(token.text);
        default:
            return "'" + std::string(token.text) + "'";
    }
}

/// Reads tokens one at a time and builds the tree by recursive descent. Errors are raised at the
/// first problem from the left: a token is read only when the one before it has been accepted.
class Parser {
public:
    explicit Parser(std::string_view text) noexcept : text_(text) {}

    WhenNode parse() {
        if (text_.find_first_not_of(" \t\r\n") == std::string_view::npos) {
            fail(0, "the expression is empty");
        }
        advance();
        WhenNode node = parse_or();
        if (current_.kind == TokenKind::close) {
            fail(current_.offset, "found ')' without a matching '('");
        }
        if (current_.kind != TokenKind::end) {
            fail_after_operand("expected '&&', '||' or the end of the expression");
        }
        return node;
    }

private:
    /// Counts the levels of '(' and '!' around the operand being parsed.
    class Nesting {
    public:
        Nesting(Parser& parser, std::size_t offset) : parser_(parser) {
            if (++parser_.depth_ > max_when_depth) {
                parser_.fail(offset, "the expression is nested deeper than " +
                                         std::to_string(max_when_depth) + " levels");
            }
        }
        Nesting(const Nesting&) = delete;
        Nesting& operator=(const Nesting&) = delete;
        ~Nesting() { --parser_.depth_; }

    private:
        Parser& parser_;
    };

    // --- Grammar ---------------------------------------------------------------------------------

    WhenNode parse_or() { return parse_chain(TokenKind::or_op, Op::any, &Parser::parse_and); }
    WhenNode parse_and() { return parse_chain(TokenKind::and_op, Op::all, &Parser::parse_unary); }

    WhenNode parse_chain(TokenKind separator, Op op, WhenNode (Parser::*operand)()) {
        WhenNode first = (this->*operand)();
        if (current_.kind != separator) {
            return first;
        }
        WhenNode chain{.op = op};
        append(chain, std::move(first));
        while (current_.kind == separator) {
            advance();
            append(chain, (this->*operand)());
        }
        return chain;
    }

    /// Adds an operand to a chain; a chain of the same operator is merged into it.
    static void append(WhenNode& chain, WhenNode operand) {
        if (operand.op != chain.op) {
            chain.operands.push_back(std::move(operand));
            return;
        }
        for (WhenNode& inner : operand.operands) {
            chain.operands.push_back(std::move(inner));
        }
    }

    WhenNode parse_unary() {
        if (current_.kind == TokenKind::not_op) {
            const Nesting nesting(*this, current_.offset);
            advance();
            return negate(parse_negated());
        }
        return parse_primary();
    }

    /// What '!' applies to: no comparisons, so that `!a == 'x'` is not silently `(!a) == 'x'`.
    WhenNode parse_negated() {
        switch (current_.kind) {
            case TokenKind::not_op: {
                const Nesting nesting(*this, current_.offset);
                advance();
                return negate(parse_negated());
            }
            case TokenKind::true_word:
            case TokenKind::false_word:
                return constant();
            case TokenKind::open:
                return parse_group();
            case TokenKind::key: {
                WhenNode node{.op = Op::key, .key = std::string(current_.text)};
                advance();
                negated_key_ = node.key;
                after_negated_key_ = current_.offset;
                return node;
            }
            default:
                fail_operand();
        }
    }

    WhenNode parse_primary() {
        switch (current_.kind) {
            case TokenKind::true_word:
            case TokenKind::false_word:
                return constant();
            case TokenKind::open:
                return parse_group();
            case TokenKind::key:
                break;
            default:
                fail_operand();
        }
        WhenNode node{.op = Op::key, .key = std::string(current_.text)};
        advance();
        if (current_.kind == TokenKind::equals_op || current_.kind == TokenKind::not_equals_op) {
            node.op = current_.kind == TokenKind::equals_op ? Op::equals : Op::not_equals;
            const Token comparison = current_;
            advance();
            parse_literal(comparison, node);
        } else if (current_.kind == TokenKind::in_word) {
            advance();
            if (current_.kind != TokenKind::key) {
                fail(current_.offset, "expected a key after 'in', found " + describe(current_));
            }
            node.op = Op::in;
            node.list = std::string(current_.text);
            advance();
        }
        return node;
    }

    WhenNode parse_group() {
        const Nesting nesting(*this, current_.offset);
        const std::size_t open = current_.offset;
        advance();
        WhenNode node = parse_or();
        if (current_.kind != TokenKind::close) {
            fail_after_operand("expected '&&', '||' or ')' to close '(' at column " +
                               std::to_string(column(open)));
        }
        advance();
        return node;
    }

    void parse_literal(const Token& comparison, WhenNode& node) {
        switch (current_.kind) {
            case TokenKind::text:
                node.literal = current_.text.substr(1, current_.text.size() - 2);
                break;
            case TokenKind::number:
                node.literal = current_.number;
                break;
            case TokenKind::true_word:
            case TokenKind::false_word:
                node.literal = current_.kind == TokenKind::true_word;
                break;
            default: {
                std::string reason = "expected a value after '" + std::string(comparison.text) +
                                     "' (text in single quotes, a number, true or false), found " +
                                     describe(current_);
                if (current_.kind == TokenKind::key) {
                    reason += "; write '" + std::string(current_.text) + "' to compare with text";
                }
                fail(current_.offset, reason);
            }
        }
        node.spelling = std::string(current_.text);
        advance();
    }

    WhenNode constant() {
        WhenNode node{.op = Op::constant, .constant = current_.kind == TokenKind::true_word};
        advance();
        return node;
    }

    static WhenNode negate(WhenNode operand) {
        WhenNode node{.op = Op::negate};
        node.operands.push_back(std::move(operand));
        return node;
    }

    // --- Errors ----------------------------------------------------------------------------------

    /// An operand was expected and `current_` cannot start one.
    [[noreturn]] void fail_operand() const {
        std::string reason = "expected a key, '!', '(', true or false";
        if (previous_.kind != TokenKind::end) {
            reason += " after '" + std::string(previous_.text) + "'";
        }
        reason += ", found " + describe(current_);
        if (current_.kind == TokenKind::text || current_.kind == TokenKind::number) {
            reason += "; a comparison starts with the key: key == " + std::string(current_.text);
        }
        fail(current_.offset, reason);
    }

    /// An operand is complete and `current_` cannot follow it.
    [[noreturn]] void fail_after_operand(const std::string& expected) const {
        if (is_comparison(current_.kind)) {
            if (current_.offset == after_negated_key_) {
                fail(current_.offset,
                     "'!' cannot negate a comparison without parentheses; write !(" + negated_key_ +
                         " " + std::string(current_.text) + " ...) instead");
            }
            fail(current_.offset, expected + ", found " + describe(current_) +
                                      "; the left side of a comparison must be a key");
        }
        fail(current_.offset, expected + ", found " + describe(current_));
    }

    [[noreturn]] void fail(std::size_t offset, const std::string& reason) const {
        throw WhenSyntaxError(offset, column(offset), reason);
    }

    /// 1-based column of a byte offset, in characters: UTF-8 continuation bytes do not count.
    [[nodiscard]] std::size_t column(std::size_t offset) const noexcept {
        std::size_t result = 1;
        for (std::size_t i = 0; i < offset && i < text_.size(); ++i) {
            if ((static_cast<unsigned char>(text_[i]) & 0xC0U) != 0x80U) {
                ++result;
            }
        }
        return result;
    }

    // --- Tokens ----------------------------------------------------------------------------------

    void advance() {
        previous_ = current_;
        current_ = next_token();
    }

    Token next_token() {
        while (pos_ < text_.size() && is_space(text_[pos_])) {
            ++pos_;
        }
        const std::size_t start = pos_;
        if (start == text_.size()) {
            return Token{.kind = TokenKind::end, .offset = start};
        }
        const char c = text_[start];
        const char next = start + 1 < text_.size() ? text_[start + 1] : '\0';
        switch (c) {
            case '(':
                return punctuation(TokenKind::open, 1);
            case ')':
                return punctuation(TokenKind::close, 1);
            case '&':
                if (next != '&') {
                    fail(start, "unexpected character '&'; did you mean '&&'?");
                }
                return punctuation(TokenKind::and_op, 2);
            case '|':
                if (next != '|') {
                    fail(start, "unexpected character '|'; did you mean '||'?");
                }
                return punctuation(TokenKind::or_op, 2);
            case '=':
                if (next != '=') {
                    fail(start, "unexpected character '='; did you mean '=='?");
                }
                return punctuation(TokenKind::equals_op, 2);
            case '!':
                return next == '=' ? punctuation(TokenKind::not_equals_op, 2)
                                   : punctuation(TokenKind::not_op, 1);
            case '\'':
                return text();
            case '"':
                fail(start, "unexpected character '\"'; text is written in single quotes");
            default:
                break;
        }
        if (is_ascii_digit(c) || (c == '-' && is_ascii_digit(next))) {
            return number();
        }
        if (is_key_start(c)) {
            return word();
        }
        fail(start, "unexpected " + describe_character(start));
    }

    Token punctuation(TokenKind kind, std::size_t length) {
        const Token token{.kind = kind, .offset = pos_, .text = text_.substr(pos_, length)};
        pos_ += length;
        return token;
    }

    /// Text in single quotes, without escapes.
    Token text() {
        const std::size_t start = pos_;
        const std::size_t close = text_.find('\'', start + 1);
        if (close == std::string_view::npos) {
            fail(start, "the text has no closing quote '");
        }
        if (!model::detail::is_valid_utf8(text_.substr(start + 1, close - start - 1))) {
            fail(start, "the text is not valid UTF-8");
        }
        pos_ = close + 1;
        return Token{
            .kind = TokenKind::text, .offset = start, .text = text_.substr(start, pos_ - start)};
    }

    Token number() {
        const std::size_t start = pos_;
        // Take everything that could belong to the number, so that the error shows all of it.
        std::size_t end = start + 1;
        while (end < text_.size()) {
            const char c = text_[end];
            const bool sign_of_exponent =
                (c == '+' || c == '-') && (text_[end - 1] == 'e' || text_[end - 1] == 'E');
            if (!is_ascii_digit(c) && !is_ascii_letter(c) && c != '_' && c != '.' &&
                !sign_of_exponent) {
                break;
            }
            ++end;
        }
        const std::string_view spelling = text_.substr(start, end - start);
        if (!is_number(spelling)) {
            fail(start, "invalid number '" + std::string(spelling) + "'");
        }
        const std::optional<double> value = fem::detail::parse_double(spelling);
        if (!value || !std::isfinite(*value)) {
            fail(start, "the number '" + std::string(spelling) + "' is out of range");
        }
        pos_ = end;
        return Token{
            .kind = TokenKind::number, .offset = start, .text = spelling, .number = *value};
    }

    /// A key, or one of the words true, false and in.
    Token word() {
        const std::size_t start = pos_;
        std::size_t end = start;
        while (end < text_.size() && is_key_char(text_[end])) {
            ++end;
        }
        const std::string_view spelling = text_.substr(start, end - start);
        if (spelling.back() == '.' || spelling.find("..") != std::string_view::npos) {
            fail(start, "invalid key '" + std::string(spelling) +
                            "': every '.' must be followed by a name");
        }
        pos_ = end;
        TokenKind kind = TokenKind::key;
        if (spelling == "true") {
            kind = TokenKind::true_word;
        } else if (spelling == "false") {
            kind = TokenKind::false_word;
        } else if (spelling == "in") {
            kind = TokenKind::in_word;
        }
        return Token{.kind = kind, .offset = start, .text = spelling};
    }

    /// "character 'x'", "character U+0001" or "byte 0xFF, not UTF-8".
    [[nodiscard]] std::string describe_character(std::size_t offset) const {
        const auto lead = static_cast<unsigned char>(text_[offset]);
        if (lead < 0x20U || lead == 0x7FU) {
            return "character U+" + hex(lead, 4);
        }
        if (lead < 0x80U) {
            return "character '" + std::string(1, static_cast<char>(lead)) + "'";
        }
        std::size_t length = 0;
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
        }
        const std::string_view sequence = text_.substr(offset, length);
        if (length != 0 && sequence.size() == length && model::detail::is_valid_utf8(sequence)) {
            return "character '" + std::string(sequence) + "'";
        }
        return "byte 0x" + hex(lead, 2) + ", not UTF-8";
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    Token current_{};
    Token previous_{};  // Of kind `end` before the first token is accepted.
    std::size_t depth_ = 0;
    // The key of the last `!key` and the offset of the token after it.
    std::string negated_key_;
    std::size_t after_negated_key_ = std::string_view::npos;
};

}  // namespace

WhenNode parse_when(std::string_view text) {
    return Parser(text).parse();
}

}  // namespace fem::context::detail
