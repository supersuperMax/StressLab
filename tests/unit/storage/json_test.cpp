#include "fem/storage/json.h"

#include "fem/core/text/number.h"
#include "fem/storage/error.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>

namespace fem::storage {
namespace {

using Json = nlohmann::json;

ProjectErrc error_code(const std::function<void()>& action) {
    try {
        action();
    } catch (const ProjectError& error) {
        return error.code();
    }
    ADD_FAILURE() << "no ProjectError";
    return ProjectErrc::io;
}

TEST(StorageJson, CanonicalLayout) {
    const Json value = Json::parse(R"({"b": [1, {"y": null, "x": true}], "a": {}, "c": []})");
    EXPECT_EQ(to_canonical_json(value),
              "{\n"
              "  \"a\": {},\n"
              "  \"b\": [\n"
              "    1,\n"
              "    {\n"
              "      \"x\": true,\n"
              "      \"y\": null\n"
              "    }\n"
              "  ],\n"
              "  \"c\": []\n"
              "}\n");
    EXPECT_EQ(to_canonical_json(Json::object()), "{}\n");
    EXPECT_EQ(to_canonical_json(Json("x")), "\"x\"\n");
}

TEST(StorageJson, KeysAreSortedByCodePoint) {
    const Json value = {{"я", 1}, {"Z", 2}, {"a", 3}, {"é", 4}, {"😀", 5}, {"", 6}};
    EXPECT_EQ(
        to_canonical_json(value),
        "{\n  \"\": 6,\n  \"Z\": 2,\n  \"a\": 3,\n  \"é\": 4,\n  \"я\": 1,\n  \"😀\": 5\n}\n");
}

TEST(StorageJson, NumbersAreShortestAndKeepTheirKind) {
    const auto text = [](const Json& value) {
        std::string out = to_canonical_json(value);
        out.pop_back();
        return out;
    };
    EXPECT_EQ(text(1.0), "1.0");
    EXPECT_EQ(text(1), "1");
    EXPECT_EQ(text(0.1), "0.1");
    EXPECT_EQ(text(-0.0), "-0.0");
    EXPECT_EQ(text(1e100), "1e+100");
    EXPECT_EQ(text(1e16), "1e+16");
    EXPECT_EQ(text(123456789012345.0), "123456789012345.0");
    EXPECT_EQ(text(0.0001), "0.0001");
    EXPECT_EQ(text(0.00001), "1e-05");
    EXPECT_EQ(text(5e-324), "5e-324");
    EXPECT_EQ(text(std::numeric_limits<double>::max()), "1.7976931348623157e+308");
    EXPECT_EQ(text(std::numeric_limits<std::int64_t>::min()), "-9223372036854775808");
    EXPECT_EQ(text(std::numeric_limits<std::uint64_t>::max()), "18446744073709551615");
    EXPECT_EQ(text(210000.0), "210000.0");
}

TEST(StorageJson, EveryFiniteDoubleReadsBack) {
    for (const double value :
         {0.1, 1.0 / 3.0, -2.5e-308, 4.9406564584124654e-324, 6.02214076e23, -1.0, 1e21, 123.456}) {
        const Json read = parse_json(to_canonical_json(value), "x.json");
        ASSERT_TRUE(read.is_number_float());
        EXPECT_EQ(std::signbit(read.get<double>()), std::signbit(value));
        EXPECT_EQ(read.get<double>(), value);
    }
}

TEST(StorageJson, TextIsUtf8WithFewEscapes) {
    EXPECT_EQ(to_canonical_json("Сталь \"Ст3\"\\\n\t\x01/€"),
              "\"Сталь \\\"Ст3\\\"\\\\\\n\\t\\u0001/€\"\n");
}

TEST(StorageJson, RefusesWhatHasNoCanonicalText) {
    EXPECT_EQ(error_code([] { (void)to_canonical_json(std::nan("")); }), ProjectErrc::invalid_data);
    EXPECT_EQ(error_code([] { (void)to_canonical_json(std::numeric_limits<double>::infinity()); }),
              ProjectErrc::invalid_data);
    EXPECT_EQ(error_code([] { (void)to_canonical_json(Json{{"a", "\xff"}}); }),
              ProjectErrc::invalid_data);
    EXPECT_EQ(error_code([] { (void)to_canonical_json(Json::binary({1, 2})); }),
              ProjectErrc::invalid_data);
    EXPECT_THROW((void)text::format_double(std::nan("")), std::domain_error);
}

TEST(StorageJson, ReadsByteOrderMarksAndWindowsLineBreaks) {
    const Json value = parse_json("\xEF\xBB\xBF{\r\n  \"a\": 1.0\r\n}\r\n", "x.json");
    EXPECT_EQ(to_canonical_json(value), "{\n  \"a\": 1.0\n}\n");
}

TEST(StorageJson, SyntaxErrorsNameTheFileAndLine) {
    try {
        (void)parse_json("{\n  \"a\": 1,\n  \"b\": oops\n}\n", "model/mesh/3.json");
        FAIL() << "no error";
    } catch (const ProjectError& error) {
        EXPECT_EQ(error.code(), ProjectErrc::invalid_file);
        EXPECT_EQ(error.file(), "model/mesh/3.json");
        EXPECT_EQ(error.line(), 3);
        EXPECT_NE(std::string(error.what()).find("model/mesh/3.json:3"), std::string::npos);
    }
}

}  // namespace
}  // namespace fem::storage
