#include "fem/core/extension/semver.h"

#include "fem/core/extension/error.h"

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

namespace fem::extension {

namespace {

[[noreturn]] void fail(std::string_view text, const std::string& reason) {
    throw ExtensionError(ExtensionErrc::invalid_version,
                         "'" + std::string(text) + "' is not valid: " + reason);
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_identifier_char(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

bool is_space(char c) {
    return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::string describe_char(char c) {
    return c == '\0' ? std::string("the end") : "'" + std::string(1, c) + "'";
}

/// Reads a number without leading zeros at `pos` of `text` (`whole` is for messages).
std::uint64_t read_number(std::string_view text, std::size_t& pos, std::string_view whole) {
    const std::size_t start = pos;
    while (pos < text.size() && is_digit(text[pos])) {
        ++pos;
    }
    if (pos == start) {
        fail(whole,
             "expected a number, found " + describe_char(start < text.size() ? text[start] : '\0'));
    }
    const std::string_view digits = text.substr(start, pos - start);
    if (digits.size() > 1 && digits[0] == '0') {
        fail(whole, "the number '" + std::string(digits) + "' has a leading zero");
    }
    std::uint64_t value = 0;
    for (const char c : digits) {
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            fail(whole, "the number '" + std::string(digits) + "' is too large");
        }
        value = value * 10 + digit;
    }
    return value;
}

/// Checks dot-separated identifiers of a pre-release or a build.
void check_identifiers(std::string_view identifiers, bool numeric_without_zeros,
                       std::string_view what, std::string_view whole) {
    std::size_t start = 0;
    while (true) {
        const std::size_t end = identifiers.find('.', start);
        const std::string_view identifier = identifiers.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (identifier.empty()) {
            fail(whole, "empty identifier in the " + std::string(what));
        }
        bool numeric = true;
        for (const char c : identifier) {
            if (!is_identifier_char(c)) {
                fail(whole, "invalid character " + describe_char(c) + " in the " +
                                std::string(what) + "; use ASCII letters, digits and '-'");
            }
            numeric = numeric && is_digit(c);
        }
        if (numeric_without_zeros && numeric && identifier.size() > 1 && identifier[0] == '0') {
            fail(whole, "the numeric identifier '" + std::string(identifier) + "' of the " +
                            std::string(what) + " has a leading zero");
        }
        if (end == std::string_view::npos) {
            return;
        }
        start = end + 1;
    }
}

/// A version with one, two or three numbers, as ranges write them; only a full version may have
/// a pre-release and build metadata.
struct Partial {
    std::array<std::uint64_t, 3> numbers{};
    int count = 0;
    std::string prerelease;
    std::string build;
};

Partial read_partial(std::string_view text, std::string_view whole) {
    Partial partial;
    std::size_t pos = 0;
    while (true) {
        partial.numbers[static_cast<std::size_t>(partial.count)] = read_number(text, pos, whole);
        ++partial.count;
        if (partial.count == 3 || pos == text.size() || text[pos] != '.') {
            break;
        }
        ++pos;
    }
    if (partial.count == 3 && pos < text.size() && text[pos] == '-') {
        const std::size_t end = text.find('+', pos);
        partial.prerelease = std::string(text.substr(
            pos + 1, end == std::string_view::npos ? std::string_view::npos : end - pos - 1));
        check_identifiers(partial.prerelease, true, "pre-release", whole);
        pos = end == std::string_view::npos ? text.size() : end;
    }
    if (partial.count == 3 && pos < text.size() && text[pos] == '+') {
        partial.build = std::string(text.substr(pos + 1));
        check_identifiers(partial.build, false, "build metadata", whole);
        pos = text.size();
    }
    if (pos != text.size()) {
        fail(whole, "unexpected " + describe_char(text[pos]) + " after " +
                        std::string(text.substr(0, pos)));
    }
    return partial;
}

/// Compares pre-releases by Semantic Versioning: a release (empty) is above any pre-release.
std::strong_ordering compare_prerelease(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) {
        return a.empty() <=> b.empty();
    }
    std::size_t pos_a = 0;
    std::size_t pos_b = 0;
    while (pos_a <= a.size() && pos_b <= b.size()) {
        const std::size_t end_a = std::min(a.find('.', pos_a), a.size());
        const std::size_t end_b = std::min(b.find('.', pos_b), b.size());
        const std::string_view id_a = a.substr(pos_a, end_a - pos_a);
        const std::string_view id_b = b.substr(pos_b, end_b - pos_b);
        const bool numeric_a = id_a.find_first_not_of("0123456789") == std::string_view::npos;
        const bool numeric_b = id_b.find_first_not_of("0123456789") == std::string_view::npos;
        std::strong_ordering order = std::strong_ordering::equal;
        if (numeric_a && numeric_b) {
            // No leading zeros: a longer number is larger.
            order = id_a.size() <=> id_b.size();
            if (order == std::strong_ordering::equal) {
                order = id_a <=> id_b;
            }
        } else if (numeric_a != numeric_b) {
            order = numeric_a ? std::strong_ordering::less : std::strong_ordering::greater;
        } else {
            order = id_a <=> id_b;
        }
        if (order != std::strong_ordering::equal) {
            return order;
        }
        pos_a = end_a + 1;
        pos_b = end_b + 1;
    }
    // Equal so far: the one with more identifiers is larger.
    return (pos_a <= a.size()) <=> (pos_b <= b.size());
}

}  // namespace

// --- Version -------------------------------------------------------------------------------------

Version Version::parse(std::string_view text) {
    Partial partial = read_partial(text, text);
    if (partial.count != 3) {
        fail(text, "expected major.minor.patch");
    }
    Version version(partial.numbers[0], partial.numbers[1], partial.numbers[2]);
    version.prerelease_ = std::move(partial.prerelease);
    version.build_ = std::move(partial.build);
    return version;
}

std::string Version::to_string() const {
    std::string text =
        std::to_string(major_) + '.' + std::to_string(minor_) + '.' + std::to_string(patch_);
    if (!prerelease_.empty()) {
        text += '-' + prerelease_;
    }
    if (!build_.empty()) {
        text += '+' + build_;
    }
    return text;
}

std::strong_ordering operator<=>(const Version& a, const Version& b) noexcept {
    if (const auto order = a.major_ <=> b.major_; order != std::strong_ordering::equal) {
        return order;
    }
    if (const auto order = a.minor_ <=> b.minor_; order != std::strong_ordering::equal) {
        return order;
    }
    if (const auto order = a.patch_ <=> b.patch_; order != std::strong_ordering::equal) {
        return order;
    }
    return compare_prerelease(a.prerelease_, b.prerelease_);
}

// --- VersionRange --------------------------------------------------------------------------------

namespace {

enum class RangeOp : std::uint8_t {
    none,
    caret,
    tilde,
    equal,
    greater,
    greater_equal,
    less,
    less_equal
};

/// The operator at the start of `token`, and its length.
std::pair<RangeOp, std::size_t> read_op(std::string_view token) {
    if (token.starts_with(">=")) {
        return {RangeOp::greater_equal, 2};
    }
    if (token.starts_with("<=")) {
        return {RangeOp::less_equal, 2};
    }
    if (!token.empty()) {
        switch (token[0]) {
            case '^':
                return {RangeOp::caret, 1};
            case '~':
                return {RangeOp::tilde, 1};
            case '=':
                return {RangeOp::equal, 1};
            case '>':
                return {RangeOp::greater, 1};
            case '<':
                return {RangeOp::less, 1};
            default:
                break;
        }
    }
    return {RangeOp::none, 0};
}

std::uint64_t next(std::uint64_t value, std::string_view whole) {
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        fail(whole, "a version number is too large");
    }
    return value + 1;
}

}  // namespace

VersionRange::VersionRange() : sets_(1), text_("*") {}

VersionRange VersionRange::parse(std::string_view text) {
    using Op = Comparator::Op;
    VersionRange range;
    range.sets_.clear();
    range.text_ = std::string(trim(text));
    if (range.text_.empty()) {
        fail(text, "the range is empty; write '*' for any version");
    }
    const std::string_view whole = range.text_;
    std::size_t start = 0;
    while (true) {
        const std::size_t bar = whole.find("||", start);
        const std::string_view alternative = trim(whole.substr(
            start, bar == std::string_view::npos ? std::string_view::npos : bar - start));
        if (alternative.empty()) {
            fail(whole, "expected a version range around '||'");
        }
        Set set;
        std::size_t pos = 0;
        while (pos < alternative.size()) {
            while (pos < alternative.size() && is_space(alternative[pos])) {
                ++pos;
            }
            const auto [op, op_length] = read_op(alternative.substr(pos));
            pos += op_length;
            while (op != RangeOp::none && pos < alternative.size() && is_space(alternative[pos])) {
                ++pos;
            }
            std::size_t end = pos;
            while (end < alternative.size() && !is_space(alternative[end])) {
                ++end;
            }
            const std::string_view token = alternative.substr(pos, end - pos);
            pos = end;
            if (token == "*" && op == RangeOp::none) {
                continue;
            }
            if (token.empty()) {
                fail(whole, "expected a version after the operator");
            }
            const Partial partial = read_partial(token, whole);
            const std::uint64_t major = partial.numbers[0];
            const std::uint64_t minor = partial.numbers[1];
            const std::uint64_t patch = partial.numbers[2];
            Version lower(major, minor, patch);
            lower.prerelease_ = partial.prerelease;
            const auto add = [&set](Op comparison, Version version) {
                set.push_back({comparison, std::move(version)});
            };
            // Upper bound of the versions that start with the partial version.
            const auto bump_partial = [&]() {
                return partial.count == 1 ? Version(next(major, whole), 0, 0)
                                          : Version(major, next(minor, whole), 0);
            };
            switch (op) {
                case RangeOp::none:
                case RangeOp::equal:
                    if (partial.count == 3) {
                        add(Op::equal, lower);
                    } else {
                        add(Op::greater_equal, lower);
                        add(Op::less, bump_partial());
                    }
                    break;
                case RangeOp::caret:
                    add(Op::greater_equal, lower);
                    if (partial.count == 1 || major > 0) {
                        add(Op::less, Version(next(major, whole), 0, 0));
                    } else if (partial.count == 2 || minor > 0) {
                        add(Op::less, Version(0, next(minor, whole), 0));
                    } else {
                        add(Op::less, Version(0, 0, next(patch, whole)));
                    }
                    break;
                case RangeOp::tilde:
                    add(Op::greater_equal, lower);
                    add(Op::less, partial.count == 1 ? Version(next(major, whole), 0, 0)
                                                     : Version(major, next(minor, whole), 0));
                    break;
                case RangeOp::greater:
                    if (partial.count == 3) {
                        add(Op::greater, lower);
                    } else {
                        add(Op::greater_equal, bump_partial());
                    }
                    break;
                case RangeOp::greater_equal:
                    add(Op::greater_equal, lower);
                    break;
                case RangeOp::less:
                    add(Op::less, lower);
                    break;
                case RangeOp::less_equal:
                    if (partial.count == 3) {
                        add(Op::less_equal, lower);
                    } else {
                        add(Op::less, bump_partial());
                    }
                    break;
            }
        }
        range.sets_.push_back(std::move(set));
        if (bar == std::string_view::npos) {
            break;
        }
        start = bar + 2;
    }
    return range;
}

bool VersionRange::contains(const Version& version) const {
    using Op = Comparator::Op;
    const auto satisfies = [&version](const Comparator& comparator) {
        const std::strong_ordering order = version <=> comparator.version;
        switch (comparator.op) {
            case Op::less:
                return order == std::strong_ordering::less;
            case Op::less_equal:
                return order != std::strong_ordering::greater;
            case Op::greater:
                return order == std::strong_ordering::greater;
            case Op::greater_equal:
                return order != std::strong_ordering::less;
            case Op::equal:
                return order == std::strong_ordering::equal;
        }
        return false;
    };
    const auto names_prerelease_of = [&version](const Comparator& comparator) {
        const Version& bound = comparator.version;
        return bound.is_prerelease() && bound.major() == version.major() &&
               bound.minor() == version.minor() && bound.patch() == version.patch();
    };
    for (const Set& set : sets_) {
        bool all = true;
        for (const Comparator& comparator : set) {
            all = all && satisfies(comparator);
        }
        if (!all) {
            continue;
        }
        if (!version.is_prerelease()) {
            return true;
        }
        for (const Comparator& comparator : set) {
            if (names_prerelease_of(comparator)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace fem::extension
