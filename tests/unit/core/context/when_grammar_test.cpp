#include "fem/core/context/error.h"
#include "fem/core/context/when.h"

#include "context_test_support.h"
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {

using fem::context::ContextErrc;
using fem::context::is_valid_key;
using fem::context::WhenExpression;
using fem::context::WhenSyntaxError;
using fem::context::testing::utf8;

struct NormalFormCase {
    std::string text;
    std::string normal;
};

TEST(WhenGrammar, ParsesIntoTheNormalForm) {
    const std::vector<NormalFormCase> cases = {
        {"a", "a"},
        {"  model.hasMesh\t", "model.hasMesh"},
        {"a&&b", "a && b"},
        {"a\t&&\r\nb", "a && b"},
        {"a||b", "a || b"},
        // '&&' binds tighter than '||': these parentheses add nothing and disappear.
        {"a || (b && c)", "a || b && c"},
        {"(a && b) || c", "a && b || c"},
        // ...and these change the meaning and stay.
        {"(a || b) && c", "(a || b) && c"},
        {"a && (b || c) && d", "a && (b || c) && d"},
        // Chains of one operator are one node.
        {"(a && b) && c", "a && b && c"},
        {"a || (b || c)", "a || b || c"},
        {"((a))", "a"},
        {"!a", "!a"},
        {"! a", "!a"},
        {"!!a", "!!a"},
        {"!(a)", "!a"},
        {"!(a && b)", "!(a && b)"},
        {"!(a || b) && c", "!(a || b) && c"},
        {"!(stage == 'mesh')", "!(stage == 'mesh')"},
        {"!(a in b)", "!(a in b)"},
        {"!true", "!true"},
        {"true", "true"},
        {"false || a", "false || a"},
        {"stage=='solve'", "stage == 'solve'"},
        {"stage != 'solve'", "stage != 'solve'"},
        {"name == ''", "name == ''"},
        {"name == 'two  spaces'", "name == 'two  spaces'"},
        {R"(name == 'say "hi"')", R"(name == 'say "hi"')"},
        {utf8(u8"stage == 'сетка'"), utf8(u8"stage == 'сетка'")},
        // Numbers are kept as written.
        {"count == 2", "count == 2"},
        {"count == -1", "count == -1"},
        {"count==-1", "count == -1"},
        {"count == 2.50", "count == 2.50"},
        {"count == 007", "count == 007"},
        {"x == 1e3", "x == 1e3"},
        {"x == 1.5E-3", "x == 1.5E-3"},
        {"x == 2e+2", "x == 2e+2"},
        {"flag == true", "flag == true"},
        {"flag != false", "flag != false"},
        {"selection.type in solver.my-solver.types", "selection.type in solver.my-solver.types"},
        // Keys: dots, digits, '_' and '-'; the words of the language only as whole keys.
        {"_x.y_z-1", "_x.y_z-1"},
        {"view.3d.visible", "view.3d.visible"},
        {"a-1", "a-1"},
        {"a.in && true.false", "a.in && true.false"},
        {"truex || inside", "truex || inside"},
        {"model.hasMesh && solver.demo.available && stage == 'solve' && !job.running",
         "model.hasMesh && solver.demo.available && stage == 'solve' && !job.running"},
    };
    for (const NormalFormCase& c : cases) {
        SCOPED_TRACE(c.text);
        const WhenExpression expression = WhenExpression::parse(c.text);
        EXPECT_EQ(expression.to_string(), c.normal);
        // The normal form reads back to itself.
        EXPECT_EQ(WhenExpression::parse(c.normal).to_string(), c.normal);
    }
}

TEST(WhenGrammar, KeysAreSortedAndUnique) {
    const WhenExpression expression =
        WhenExpression::parse("b && a || !c && b == 'x' && a in d && !(e != 1)");
    EXPECT_EQ(expression.keys(), (std::vector<std::string>{"a", "b", "c", "d", "e"}));
    EXPECT_TRUE(WhenExpression::parse("true && !false").keys().empty());
}

TEST(WhenGrammar, CopiesShareTheTree) {
    const WhenExpression original = WhenExpression::parse("a && b");
    const WhenExpression copy = original;  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(copy.to_string(), "a && b");
    EXPECT_EQ(copy.keys(), original.keys());
}

TEST(WhenGrammar, ValidKeys) {
    for (const char* key : {"a", "A", "_", "_x", "model.hasMesh", "solver.my-solver.available",
                            "a.1", "a-b", "a.-b", "x.true", "inside", "True"}) {
        EXPECT_TRUE(is_valid_key(key)) << key;
    }
    for (const char* key :
         {"", "1a", "-a", ".a", "a.", "a..b", "a b", "a/b", "a:b", "a$", "true", "false", "in"}) {
        EXPECT_FALSE(is_valid_key(key)) << key;
    }
    EXPECT_FALSE(is_valid_key(utf8(u8"сетка")));
}

struct ErrorCase {
    std::string text;
    std::size_t column;
    std::string reason;
};

TEST(WhenGrammar, SyntaxErrorsGiveTheColumnAndTheReason) {
    const std::string operand = "expected a key, '!', '(', true or false";
    const std::string value =
        "expected a value after '==' (text in single quotes, a number, true or false)";
    const std::string next = "expected '&&', '||' or the end of the expression";
    const std::string end = "the end of the expression";
    const std::vector<ErrorCase> cases = {
        {"", 1, "the expression is empty"},
        {" \t ", 1, "the expression is empty"},
        // A missing operand.
        {"a &&", 5, operand + " after '&&', found " + end},
        {"&& a", 1, operand + ", found '&&'"},
        {"a || || b", 6, operand + " after '||', found '||'"},
        {"()", 2, operand + " after '(', found ')'"},
        {"!", 2, operand + " after '!', found " + end},
        {"in", 1, operand + ", found 'in'"},
        {"'x' == a", 1, operand + ", found text 'x'; a comparison starts with the key: key == 'x'"},
        {"1 == a", 1, operand + ", found number 1; a comparison starts with the key: key == 1"},
        // Something after a complete operand.
        {"a b", 3, next + ", found key 'b'"},
        {"a == 'x' 'y'", 10, next + ", found text 'y'"},
        {"a == 'x' == 'y'", 10, next + ", found '=='; the left side of a comparison must be a key"},
        {"true == a", 6, next + ", found '=='; the left side of a comparison must be a key"},
        {"a in b in c", 8, next + ", found 'in'; the left side of a comparison must be a key"},
        {"!a == 'x'", 4,
         "'!' cannot negate a comparison without parentheses; write !(a == ...) instead"},
        {"x && !a != 1", 9,
         "'!' cannot negate a comparison without parentheses; write !(a != ...) instead"},
        {"!a in b", 4,
         "'!' cannot negate a comparison without parentheses; write !(a in ...) instead"},
        // Parentheses.
        {"(a", 3, "expected '&&', '||' or ')' to close '(' at column 1, found " + end},
        {"(a && (b)", 10, "expected '&&', '||' or ')' to close '(' at column 1, found " + end},
        {"x && !(a b)", 10, "expected '&&', '||' or ')' to close '(' at column 7, found key 'b'"},
        {"a)", 2, "found ')' without a matching '('"},
        {"(a))", 4, "found ')' without a matching '('"},
        // Right side of comparisons.
        {"stage == solve", 10, value + ", found key 'solve'; write 'solve' to compare with text"},
        {"stage ==", 9, value + ", found " + end},
        {"stage != (a)", 10,
         "expected a value after '!=' (text in single quotes, a number, true or false), "
         "found '('"},
        {"a in 'x'", 6, "expected a key after 'in', found text 'x'"},
        {"a in", 5, "expected a key after 'in', found " + end},
        // Characters that start no token.
        {"a & b", 3, "unexpected character '&'; did you mean '&&'?"},
        {"a | b", 3, "unexpected character '|'; did you mean '||'?"},
        {"a = 'x'", 3, "unexpected character '='; did you mean '=='?"},
        {"a == \"x\"", 6, "unexpected character '\"'; text is written in single quotes"},
        {"a ? b", 3, "unexpected character '?'"},
        {"x == -", 6, "unexpected character '-'"},
        {"x == .5", 6, "unexpected character '.'"},
        {"a\x01", 2, "unexpected character U+0001"},
        {utf8(u8"стадия"), 1, utf8(u8"unexpected character 'с'")},
        {"\xFF", 1, "unexpected byte 0xFF, not UTF-8"},
        // Text, numbers and keys.
        {"a == 'x", 6, "the text has no closing quote '"},
        {"a == 'x\xFF'", 6, "the text is not valid UTF-8"},
        {"x == 1.2.3", 6, "invalid number '1.2.3'"},
        {"x == 1.", 6, "invalid number '1.'"},
        {"x == 1e", 6, "invalid number '1e'"},
        {"x == 12abc", 6, "invalid number '12abc'"},
        {"x == 1e999", 6, "the number '1e999' is out of range"},
        {"a. && b", 1, "invalid key 'a.': every '.' must be followed by a name"},
        {"x || a..b", 6, "invalid key 'a..b': every '.' must be followed by a name"},
    };
    for (const ErrorCase& c : cases) {
        SCOPED_TRACE(c.text);
        try {
            (void)WhenExpression::parse(c.text);
            ADD_FAILURE() << "parsed without an error";
        } catch (const WhenSyntaxError& error) {
            EXPECT_EQ(error.code(), ContextErrc::syntax);
            EXPECT_EQ(error.column(), c.column);
            EXPECT_EQ(error.reason(), c.reason);
            EXPECT_EQ(std::string(error.what()),
                      "column " + std::to_string(c.column) + ": " + c.reason);
        }
    }
}

TEST(WhenGrammar, ColumnsCountCharactersAndOffsetsCountBytes) {
    // Five Cyrillic letters take ten bytes.
    const std::string text = utf8(u8"stage == 'сетка' && x y");
    try {
        (void)WhenExpression::parse(text);
        FAIL() << "parsed without an error";
    } catch (const WhenSyntaxError& error) {
        EXPECT_EQ(error.column(), 23U);
        EXPECT_EQ(error.offset(), 27U);
        EXPECT_EQ(text.substr(error.offset()), "y");
        EXPECT_EQ(error.reason(),
                  "expected '&&', '||' or the end of the expression, found key 'y'");
    }
}

TEST(WhenGrammar, NestingIsLimited) {
    const auto nested = [](std::size_t depth, const std::string& open) {
        std::string text;
        for (std::size_t i = 0; i < depth; ++i) {
            text += open;
        }
        text += "a";
        if (open == "(") {
            text += std::string(depth, ')');
        }
        return text;
    };
    EXPECT_EQ(WhenExpression::parse(nested(100, "(")).to_string(), "a");
    EXPECT_EQ(WhenExpression::parse(nested(100, "!")).to_string(), nested(100, "!"));
    for (const std::string open : {"(", "!", "!("}) {
        SCOPED_TRACE(open);
        try {
            (void)WhenExpression::parse(nested(101, open));
            ADD_FAILURE() << "parsed without an error";
        } catch (const WhenSyntaxError& error) {
            EXPECT_EQ(error.reason(), "the expression is nested deeper than 100 levels");
        }
    }
}

TEST(WhenGrammar, TheFirstErrorFromTheLeftIsReported) {
    try {
        (void)WhenExpression::parse("a b & c");
        FAIL() << "parsed without an error";
    } catch (const WhenSyntaxError& error) {
        EXPECT_EQ(error.column(), 3U);
    }
}

}  // namespace
