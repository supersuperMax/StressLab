// The format of string catalogs (task 1.7).

#include "fem/core/l10n/catalog.h"

#include "fem/core/l10n/error.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <ostream>
#include <set>
#include <string>

namespace {

using fem::l10n::Catalog;
using fem::l10n::L10nErrc;
using fem::l10n::L10nError;
using fem::l10n::Message;
using fem::l10n::parse_catalog;
using fem::l10n::PluralCategory;

std::string catalog(const std::string& strings, const std::string& modules = {}) {
    std::string text = R"({"format": "stresslab.l10n", "formatVersion": 1, "strings": )" + strings;
    if (!modules.empty()) {
        text += R"(, "modules": )" + modules;
    }
    return text + "}";
}

TEST(Catalog, ReadsTextsAndPluralForms) {
    const Catalog parsed = parse_catalog(catalog(
        R"({"run": "Запустить", "jobs": {"one": "{count} задание", "few": "{count} задания",
            "many": "{count} заданий", "other": "{count} задания"}})",
        R"({"test.other": {"hello": "Привет"}})"));
    ASSERT_EQ(parsed.strings.size(), 2U);
    const Message& run = parsed.strings.at("run");
    EXPECT_FALSE(run.is_plural());
    EXPECT_EQ(run.form(PluralCategory::other), "Запустить");
    EXPECT_EQ(run.form(PluralCategory::one), "Запустить");
    const Message& jobs = parsed.strings.at("jobs");
    EXPECT_TRUE(jobs.is_plural());
    EXPECT_EQ(jobs.form(PluralCategory::few), "{count} задания");
    EXPECT_EQ(jobs.form(PluralCategory::many), "{count} заданий");
    EXPECT_EQ(jobs.form(PluralCategory::two), "{count} задания") << "missing form: 'other'";
    EXPECT_EQ(parsed.modules.at("test.other").at("hello").form(PluralCategory::other), "Привет");
}

TEST(Catalog, StringsAndModulesAreOptional) {
    const Catalog empty = parse_catalog(R"({"format": "stresslab.l10n", "formatVersion": 1})");
    EXPECT_TRUE(empty.strings.empty());
    EXPECT_TRUE(empty.modules.empty());
}

TEST(Catalog, FindsPlaceholders) {
    using Names = std::set<std::string, std::less<>>;
    EXPECT_EQ(fem::l10n::placeholders("{a} and {b_2} and {a}"), (Names{"a", "b_2"}));
    EXPECT_EQ(fem::l10n::placeholders("{{literal}} braces"), Names{});
    EXPECT_EQ(fem::l10n::placeholders("no placeholders"), Names{});
    for (const char* broken : {"{", "}", "{a", "a}", "{}", "{a b}", "{a-b}", "{{a}"}) {
        EXPECT_THROW((void)fem::l10n::placeholders(broken), L10nError) << broken;
    }
}

struct BadCatalog {
    const char* name;
    std::string text;
    const char* reason;
};

std::ostream& operator<<(std::ostream& out, const BadCatalog& c) {
    return out << c.name;
}

class BadCatalogs : public ::testing::TestWithParam<BadCatalog> {};

TEST_P(BadCatalogs, AreRefusedWithTheReason) {
    try {
        (void)parse_catalog(GetParam().text);
        FAIL() << "accepted";
    } catch (const L10nError& e) {
        EXPECT_EQ(e.code(), L10nErrc::invalid_catalog);
        EXPECT_NE(std::string(e.what()).find(GetParam().reason), std::string::npos) << e.what();
    }
}

INSTANTIATE_TEST_SUITE_P(
    Table, BadCatalogs,
    ::testing::Values(
        BadCatalog{"not json", "{", "not JSON"}, BadCatalog{"array", "[]", "not an object"},
        BadCatalog{"another format", R"({"format": "stresslab.settings", "formatVersion": 1})",
                   "'format'"},
        BadCatalog{"newer version", R"({"format": "stresslab.l10n", "formatVersion": 2})",
                   "'formatVersion'"},
        BadCatalog{"unknown key",
                   R"({"format": "stresslab.l10n", "formatVersion": 1, "string": {}})",
                   "unknown key 'string'"},
        BadCatalog{"strings not an object", catalog("[]"), "'strings' must be an object"},
        BadCatalog{"number as text", catalog(R"({"a": 1})"), "string 'a'"},
        BadCatalog{"empty key", catalog(R"({"": "x"})"), "empty key"},
        BadCatalog{"plural without other", catalog(R"({"a": {"one": "x"}})"), "'other'"},
        BadCatalog{"unknown plural category", catalog(R"({"a": {"other": "x", "several": "y"}})"),
                   "'several'"},
        BadCatalog{"plural form not text", catalog(R"({"a": {"other": 2}})"), "string 'a'"},
        BadCatalog{"broken placeholder", catalog(R"({"a": "{name"})"), "string 'a'"},
        BadCatalog{"modules not an object", catalog("{}", "[]"), "'modules'"},
        BadCatalog{"module strings not an object", catalog("{}", R"({"test.x": 1})"), "'test.x'"},
        BadCatalog{"bad string of a module", catalog("{}", R"({"test.x": {"a": null}})"),
                   "string 'a' of module 'test.x'"}));

TEST(Catalog, LoadingAMissingFileIsAnIoError) {
    try {
        (void)fem::l10n::load_catalog(std::filesystem::temp_directory_path() /
                                      std::filesystem::path(u8"нет такого.json"));
        FAIL() << "loaded";
    } catch (const L10nError& e) {
        EXPECT_EQ(e.code(), L10nErrc::io);
    }
}

}  // namespace
