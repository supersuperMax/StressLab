// Reading input files (task 3.2): numbers whatever the locale, text encodings, line breaks,
// keyword lines and *INCLUDE. Translation to the Model IR: import_test.cpp.

#include "inp/deck.h"
#include "inp/encoding.h"
#include "inp/numbers.h"
#include "inp_test_support.h"
#include <gtest/gtest.h>

#include <clocale>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace fem::inp {
namespace {

using testing::data;
using testing::refusal;
using testing::utf8;

TEST(InpNumbers, ReadTheFormsOfInputFiles) {
    EXPECT_EQ(parse_number("12"), 12.0);
    EXPECT_EQ(parse_number("-3."), -3.0);
    EXPECT_EQ(parse_number(".5"), 0.5);
    EXPECT_EQ(parse_number("+2.1e5"), 2.1e5);
    EXPECT_EQ(parse_number("2.1E+05"), 2.1e5);
    EXPECT_EQ(parse_number("7.85D-9"), 7.85e-9);
    EXPECT_EQ(parse_number("1d0"), 1.0);
    for (const char* bad :
         {"", ".", "-", "1,5", "1e", "e5", "1.0.0", "1e5x", " 1", "nan", "inf", "1e400"}) {
        EXPECT_FALSE(parse_number(bad).has_value()) << bad;
    }
    EXPECT_EQ(parse_integer("42"), 42);
    EXPECT_EQ(parse_integer("+7"), 7);
    EXPECT_EQ(parse_integer("5."), 5);
    EXPECT_EQ(parse_integer("-2.0"), -2);
    EXPECT_FALSE(parse_integer("2.5").has_value());
    EXPECT_FALSE(parse_integer("Set-1").has_value());
}

TEST(InpNumbers, DoNotDependOnTheDecimalSeparatorOfTheLocale) {
    // Qt sets LC_NUMERIC from the environment on Unix, where it may be "ru_RU" with a comma.
    const std::string before = std::setlocale(LC_NUMERIC, nullptr);
    for (const char* locale : {"ru_RU.UTF-8", "de_DE.UTF-8", "Russian_Russia.1251", "German"}) {
        if (std::setlocale(LC_NUMERIC, locale) != nullptr) {
            break;
        }
    }
    EXPECT_EQ(parse_number("1.5"), 1.5);
    EXPECT_EQ(parse_number("-0.25e1"), -2.5);
    std::setlocale(LC_NUMERIC, before.c_str());
}

TEST(InpEncoding, ByteOrderMarksWinAndUtf8IsTheDefault) {
    const std::string text = utf8(u8"Кронштейн");
    auto decoded = decode("\xEF\xBB\xBF" + text, Encoding::windows1251);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->text, text);
    EXPECT_EQ(decoded->encoding, "utf-8");

    decoded = decode(text, Encoding::automatic);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->text, text);
    EXPECT_FALSE(decoded->fallback);

    // Not UTF-8: automatic falls back to Windows-1252, utf-8 refuses.
    decoded = decode("caf\xE9", Encoding::automatic);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->fallback);
    EXPECT_EQ(decoded->text, utf8(u8"café"));
    EXPECT_FALSE(decode("caf\xE9", Encoding::utf8));
    EXPECT_FALSE(is_valid_utf8("\xC0\x80"));      // An overlong NUL.
    EXPECT_FALSE(is_valid_utf8("\xED\xA0\x80"));  // A surrogate.
    EXPECT_TRUE(is_valid_utf8(utf8(u8"€ 𝄞")));
}

TEST(InpEncoding, CodePagesGiveTheirLetters) {
    EXPECT_EQ(
        decode("\xCA\xF0\xEE\xED\xF8\xF2\xE5\xE9\xED \xB9\xA8\xB8", Encoding::windows1251)->text,
        utf8(u8"Кронштейн №Ёё"));
    EXPECT_EQ(decode("\x80\x93\x94\xFF", Encoding::windows1252)->text, utf8(u8"€“”ÿ"));
    EXPECT_EQ(decode("\x80\xE9", Encoding::latin1)->text, utf8(u8"\u0080é"));
    EXPECT_EQ(parse_encoding("Windows-1251"), Encoding::windows1251);
    EXPECT_EQ(parse_encoding("auto"), Encoding::automatic);
    EXPECT_FALSE(parse_encoding("koi8-r"));
}

TEST(InpDeck, SplitsKeywordsParametersAndDataLines) {
    const Deck deck = read_deck(data() / "flat.inp", Encoding::automatic);
    ASSERT_EQ(deck.files.size(), 1U);
    EXPECT_EQ(deck.files[0].display, "flat.inp");
    const Keyword& heading = deck.keywords.front();
    EXPECT_EQ(heading.name, "HEADING");
    EXPECT_EQ(heading.line, 3);
    ASSERT_EQ(heading.data.size(), 2U);
    EXPECT_EQ(heading.data[1].line, 5);

    const Keyword& element = deck.keywords[2];
    EXPECT_EQ(element.name, "ELEMENT");
    EXPECT_EQ(element.value("TYPE"), "C3D8");
    EXPECT_EQ(element.value("ELSET"), "EALL");
    EXPECT_FALSE(element.has("NSET"));
    EXPECT_EQ(element.text(),
              "*ELEMENT, TYPE=C3D8, ELSET=EALL\n1, 1, 2, 5, 4, 7, 8, 11, 10\n"
              "2, 2, 3, 6, 5, 8, 9, 12, 11");
    EXPECT_EQ(element.index, 2U);

    const auto fields = split_fields(R"( "Set 1" , 2,  ,3.5,)");
    EXPECT_EQ(fields, (std::vector<std::string_view>{"Set 1", "2", "", "3.5", ""}));
}

TEST(InpDeck, ContinuedKeywordLinesAndQuotedValues) {
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "stresslab-inp-continued.inp";
    {
        std::ofstream out(file, std::ios::binary);
        out << "*Solid  section, elset=\"Set, 1\",\n   material=Steel\n**\n,\n";
    }
    const Deck deck = read_deck(file, Encoding::automatic);
    ASSERT_EQ(deck.keywords.size(), 1U);
    const Keyword& keyword = deck.keywords[0];
    EXPECT_EQ(keyword.name, "SOLID SECTION");
    EXPECT_EQ(keyword.line, 1);
    EXPECT_EQ(keyword.value("ELSET"), "Set, 1");
    EXPECT_EQ(keyword.value("MATERIAL"), "Steel");
    EXPECT_EQ(keyword.written, "*Solid  section, elset=\"Set, 1\",\n   material=Steel");
    ASSERT_EQ(keyword.data.size(), 1U);
    EXPECT_EQ(keyword.data[0].line, 4);
    std::filesystem::remove(file);
}

TEST(InpDeck, IncludedLinesTakeThePlaceOfTheInclude) {
    const Deck deck = read_deck(data() / "include" / "main.inp", Encoding::automatic);
    ASSERT_EQ(deck.files.size(), 5U);
    EXPECT_EQ(deck.files[0].display, "main.inp");
    EXPECT_EQ(deck.files[1].display, utf8(u8"папка с пробелами/узлы.inp"));
    EXPECT_EQ(deck.files[4].display, utf8(u8"папка с пробелами/нагрузка.inp"));
    // The nodes of the included file are the data lines of *NODE of the main file.
    const Keyword& nodes = deck.keywords[1];
    EXPECT_EQ(nodes.name, "NODE");
    EXPECT_EQ(nodes.file, 0U);
    ASSERT_EQ(nodes.data.size(), 4U);
    EXPECT_EQ(nodes.data[3].line, 4);
    for (const Keyword& keyword : deck.keywords) {
        EXPECT_NE(keyword.name, "INCLUDE");
    }
    EXPECT_EQ(deck.keywords.back().name, "END STEP");
}

TEST(InpDeck, LineBreaksOfEveryKind) {
    const Deck deck = read_deck(data() / "bytes" / "line_breaks.inp", Encoding::automatic);
    ASSERT_EQ(deck.keywords.size(), 2U);
    EXPECT_EQ(deck.keywords[0].data[0].text, "Breaks");
    const Keyword& nodes = deck.keywords[1];
    ASSERT_EQ(nodes.data.size(), 4U);
    EXPECT_EQ(nodes.line, 3);
    EXPECT_EQ(nodes.data[1].line, 5);
    EXPECT_EQ(split_fields(nodes.data[0].text),
              (std::vector<std::string_view>{"1", "0.", "0.", "0."}));
}

TEST(InpDeck, RefusesFilesItCannotRead) {
    Problem problem = refusal("errors/missing_include.inp");
    EXPECT_EQ(problem.key, "error.includeMissing");
    EXPECT_EQ(problem.file, "missing_include.inp");
    EXPECT_EQ(problem.line, 6);
    EXPECT_EQ(problem.args.at("file"), "nowhere.inp");

    problem = refusal("errors/cycle_a.inp");
    EXPECT_EQ(problem.key, "error.includeCycle");
    EXPECT_EQ(problem.file, "cycle_b.inp");
    EXPECT_EQ(problem.line, 1);

    problem = refusal("errors/data_first.inp");
    EXPECT_EQ(problem.key, "error.dataBeforeKeyword");
    EXPECT_EQ(problem.line, 2);

    problem = refusal("bytes/broken_utf16.inp");
    EXPECT_EQ(problem.key, "error.badEncoding");

    problem = refusal("bytes/cp1251.inp", Encoding::utf8);
    EXPECT_EQ(problem.key, "error.badEncoding");
    EXPECT_EQ(problem.args.at("encoding"), "utf-8");

    problem = refusal(std::filesystem::path(u8"нет такого файла.inp"));
    EXPECT_EQ(problem.key, "error.fileUnreadable");
}

}  // namespace
}  // namespace fem::inp
