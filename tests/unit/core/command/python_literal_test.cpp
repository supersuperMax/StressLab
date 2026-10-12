// Values in the journal are Python literals (docs/architecture.md §4.6, item 5). The text must be
// deterministic, read back to the same value, and mean the same value to Python itself
// (python/tests/test_journal_golden.py checks the last point with a real interpreter).

#include "fem/core/command/python_literal.h"

#include "fem/core/command/error.h"
#include "fem/core/command/value.h"

#include "command_test_support.h"
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace fem::command;
using fem::command::testing::utf8;
namespace fs = std::filesystem;

namespace {

struct NumberCase {
    double value;
    const char* text;
};

// Text is Python's repr() of the same double; hex literals give the exact values.
const NumberCase number_cases[] = {
    {0.0, "0.0"},
    {-0.0, "-0.0"},
    {1.0, "1.0"},
    {-1.0, "-1.0"},
    {0x1.999999999999ap-4, "0.1"},
    {0.5, "0.5"},
    {100.0, "100.0"},
    {0x1.edd2f1a9fbe77p+6, "123.456"},
    {0x1.9a28000000000p+17, "210000.0"},
    {0x1.3333333333333p-2, "0.3"},
    {0x1.3333333333334p-2, "0.30000000000000004"},
    {0x1.5555555555555p-2, "0.3333333333333333"},
    {0x1.5555555555555p-1, "0.6666666666666666"},
    {-0x1.1126666666666p+8, "-273.15"},
    {0x1.921fb54442d18p+1, "3.141592653589793"},
    {0x1.d6f3454000000p+26, "123456789.0"},
    {0x1.18b54f22aeb00p+50, "1234567890123456.0"},
    {0x1.c6bf526340000p+49, "1000000000000000.0"},
    {0x1.1c37937e07fffp+53, "9999999999999998.0"},
    {0x1.1c37937e08000p+53, "1e+16"},
    {0x1.aa535d3d0c000p+53, "1.5e+16"},
    {0x1.56a95319d63e1p+63, "1.2345678901234567e+19"},
    {0x1.0f0cf064dd592p+73, "1e+22"},
    {0x1.fe185ca57c517p+78, "6.02214076e+23"},
    {0x1.249ad2594c37dp+332, "1e+100"},
    {0x1.fffffffffffffp+1023, "1.7976931348623157e+308"},
    {0x1.0624dd2f1a9fcp-10, "0.001"},
    {0x1.a36e2eb1c432dp-14, "0.0001"},
    {0x1.02e4b6ce5dc68p-13, "0.00012345"},
    {0x1.4f8b588e368f1p-17, "1e-05"},
    {0x1.9e3abe16fc70dp-17, "1.2345e-05"},
    {0x1.23ec6e52c3f23p-18, "4.35e-06"},
    {0x1.ad7f29abcaf48p-24, "1e-07"},
    {0x1.0db9548994f5cp-27, "7.85e-09"},
    {0x1.0000000000000p-1022, "2.2250738585072014e-308"},
    {0x0.0000000000001p-1022, "5e-324"},
};

std::vector<Value> sample_values() {
    return {
        Value(),
        Value(true),
        Value(false),
        Value(0),
        Value(-42),
        Value(std::numeric_limits<std::int64_t>::max()),
        Value(std::numeric_limits<std::int64_t>::min()),
        Value(0.1),
        Value(-0.0),
        Value(1e-300),
        Value(""),
        Value(utf8(u8"Сталь «Ст3» — 20 °C, σ = 2·10⁵ МПа, 😀")),
        Value(std::string("tab\tquote\" back\\slash\nnew\rline")),
        Value(std::string("nul\0ctl\x01\x1f\x7f", 10)),
        // U+0085, U+2028 and U+2029 as UTF-8 bytes.
        Value(std::string("next\xC2\x85"
                          "line\xE2\x80\xA8"
                          "para\xE2\x80\xA9"
                          "end")),
        Value(fs::path(u8"модели/балка 1.dat")),
        Value(fs::path(u8"/tmp/x")),
        Value(Value::List{}),
        Value(Value::Dict{}),
        Value(Value::List{1, 2.5, "три", Value::List{Value(), true}, Value::Dict{{"k", 1}}}),
        Value(Value::Dict{{"b", 1}, {utf8(u8"ключ"), Value::List{0.3}}, {"a", Value::Dict{}}}),
    };
}

}  // namespace

TEST(PythonLiteral, NumbersAreWrittenLikePythonRepr) {
    for (const NumberCase& c : number_cases) {
        EXPECT_EQ(to_python(Value(c.value)), c.text);
    }
}

TEST(PythonLiteral, NumbersReadBackToTheSameDouble) {
    for (const NumberCase& c : number_cases) {
        const Value parsed = parse_python(c.text);
        ASSERT_EQ(parsed.kind(), Value::Kind::number) << c.text;
        EXPECT_EQ(std::bit_cast<std::uint64_t>(parsed.as_number()),
                  std::bit_cast<std::uint64_t>(c.value))
            << c.text;
    }
    EXPECT_EQ(parse_python("1E5").as_number(), 100000.0);
    EXPECT_EQ(parse_python("2.").as_number(), 2.0);
    EXPECT_EQ(parse_python("-.5").as_number(), -0.5);
}

TEST(PythonLiteral, NonFiniteNumbersAreRefused) {
    EXPECT_COMMAND_ERROR((void)to_python(Value(std::numeric_limits<double>::quiet_NaN())),
                         CommandErrc::invalid_value);
    EXPECT_COMMAND_ERROR((void)to_python(Value(std::numeric_limits<double>::infinity())),
                         CommandErrc::invalid_value);
    EXPECT_COMMAND_ERROR(
        (void)to_python(Value(Value::List{-std::numeric_limits<double>::infinity()})),
        CommandErrc::invalid_value);
}

TEST(PythonLiteral, ScalarsAndIntegers) {
    EXPECT_EQ(to_python(Value()), "None");
    EXPECT_EQ(to_python(Value(true)), "True");
    EXPECT_EQ(to_python(Value(false)), "False");
    EXPECT_EQ(to_python(Value(0)), "0");
    EXPECT_EQ(to_python(Value(-17)), "-17");
    EXPECT_EQ(to_python(Value(std::numeric_limits<std::int64_t>::max())), "9223372036854775807");
    EXPECT_EQ(to_python(Value(std::numeric_limits<std::int64_t>::min())), "-9223372036854775808");
    EXPECT_EQ(to_python(Value(fem::model::ObjectId{12})), "12");
}

TEST(PythonLiteral, TextKeepsUtf8AndEscapesOnlyWhatItMust) {
    EXPECT_EQ(python_string(""), "\"\"");
    EXPECT_EQ(python_string(utf8(u8"Сталь «Ст3» 😀")), utf8(u8"\"Сталь «Ст3» 😀\""));
    EXPECT_EQ(python_string("a\"b\\c'd"), "\"a\\\"b\\\\c'd\"");
    EXPECT_EQ(python_string("1\n2\r3\t4"), "\"1\\n2\\r3\\t4\"");
    EXPECT_EQ(python_string(std::string("\0\x01\x1b\x7f", 4)), "\"\\x00\\x01\\x1b\\x7f\"");
    // C1 controls and the Unicode line and paragraph separators break lines in some tools.
    // U+0085, U+009F, U+00A0 (not a control character: kept), U+2028, U+2029 as UTF-8 bytes.
    EXPECT_EQ(python_string("a\xC2\x85"
                            "b\xC2\x9F"
                            "c\xC2\xA0"
                            "d"),
              "\"a\\x85b\\x9fc\xC2\xA0"
              "d\"");
    EXPECT_EQ(python_string("a\xE2\x80\xA8"
                            "b\xE2\x80\xA9"
                            "c"),
              "\"a\\u2028b\\u2029c\"");
}

TEST(PythonLiteral, InvalidUtf8IsRefused) {
    EXPECT_COMMAND_ERROR((void)python_string("\xC3\x28"), CommandErrc::invalid_value);
    EXPECT_COMMAND_ERROR((void)to_python(Value(std::string("\xED\xA0\x80"))),
                         CommandErrc::invalid_value);
    EXPECT_COMMAND_ERROR((void)to_python(Value(Value::Dict{{std::string("\xFF"), 1}})),
                         CommandErrc::invalid_value);
}

TEST(PythonLiteral, PathsUseForwardSlashesOnEveryOs) {
    EXPECT_EQ(to_python(Value(fs::path(u8"модели/балка 1.dat"))),
              utf8(u8"Path(\"модели/балка 1.dat\")"));
    EXPECT_EQ(to_python(Value(fs::path("/home/user/a b"))), "Path(\"/home/user/a b\")");
#ifdef _WIN32
    EXPECT_EQ(to_python(Value(fs::path(u8"C:\\Users\\Дмитрий\\расчёт 1.dat"))),
              utf8(u8"Path(\"C:/Users/Дмитрий/расчёт 1.dat\")"));
#endif
}

TEST(PythonLiteral, ListsAndDictionaries) {
    EXPECT_EQ(to_python(Value(Value::List{})), "[]");
    EXPECT_EQ(to_python(Value(Value::Dict{})), "{}");
    EXPECT_EQ(to_python(Value(Value::List{1, 2.0, "x", Value(), Value::List{true}})),
              "[1, 2.0, \"x\", None, [True]]");
    // Keys in byte order of their UTF-8 text, whatever the order of insertion.
    Value::Dict dict;
    dict[utf8(u8"я")] = 3;
    dict[utf8(u8"а")] = 2;
    dict["z"] = 1;
    dict["A"] = Value::Dict{{"y", Value::List{}}, {"x", 0.5}};
    EXPECT_EQ(to_python(Value(dict)),
              utf8(u8"{\"A\": {\"x\": 0.5, \"y\": []}, \"z\": 1, \"а\": 2, \"я\": 3}"));
}

TEST(PythonLiteral, ValuesSurviveARoundTrip) {
    for (const Value& value : sample_values()) {
        const std::string text = to_python(value);
        const Value parsed = parse_python(text);
        EXPECT_EQ(parsed, value) << text;
        EXPECT_EQ(to_python(parsed), text);
    }
}

TEST(PythonLiteral, ParserAcceptsPythonVariants) {
    EXPECT_EQ(parse_python("'single' "), Value("single"));
    EXPECT_EQ(parse_python(" [ 1 , 'a' , ] "), Value(Value::List{1, "a"}));
    EXPECT_EQ(parse_python("{'b': 2, \"a\": 1,}"), Value(Value::Dict{{"a", 1}, {"b", 2}}));
    EXPECT_EQ(parse_python("Path( 'a/b' )"), Value(fs::path("a/b")));
    EXPECT_EQ(parse_python(R"("\u0416\U0001F600\x41\'\"\a\b\f\v")"),
              Value(utf8(u8"Ж😀A'\"\a\b\f\v")));
    EXPECT_EQ(parse_python("-7"), Value(-7));
}

TEST(PythonLiteral, ParserReportsTheColumn) {
    const char* bad[] = {
        "",                     // nothing
        "\"open",               // unterminated text
        "[1, 2",                // missing ]
        "{\"a\" 1}",            // missing :
        "{1: 2}",               // keys must be text
        "1 2",                  // trailing text
        "007",                  // Python refuses leading zeros
        "9223372036854775808",  // out of int64
        "nan",                  // not a literal
        "Path(1)",              // a path needs text
        R"("\q")",              // unknown escape
        R"("\ud800")",          // lone surrogate
        "\"\xC3\x28\"",         // invalid UTF-8
        "1e999",                // not finite
    };
    for (const char* text : bad) {
        try {
            (void)parse_python(text);
            ADD_FAILURE() << "parsed: " << text;
        } catch (const CommandError& error) {
            EXPECT_EQ(error.code(), CommandErrc::journal_syntax) << text;
            EXPECT_NE(std::string(error.what()).find("column"), std::string::npos) << error.what();
        }
    }
}
