// Substitution of translations: placeholders, plural forms, languages and their fallbacks
// (task 1.7).

#include "fem/core/l10n/localization.h"

#include "fem/core/l10n/catalog.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using fem::l10n::Args;
using fem::l10n::Catalog;
using fem::l10n::format;
using fem::l10n::Localization;
using fem::l10n::parse_catalog;
using fem::l10n::plural_category;
using fem::l10n::PluralCategory;
using Strings = std::vector<std::string>;

Catalog catalog(const std::string& strings, const std::string& modules = "{}") {
    return parse_catalog(R"({"format": "stresslab.l10n", "formatVersion": 1, "strings": )" +
                         strings + R"(, "modules": )" + modules + "}");
}

TEST(Format, PutsArgumentsIntoPlaceholders) {
    const Args args = {{"name", std::string("Мир")}, {"count", std::int64_t{3}}, {"x", 0.1}};
    EXPECT_EQ(format("Привет, {name}!", args), "Привет, Мир!");
    EXPECT_EQ(format("{count} × {x} = {count}{x}", args), "3 × 0.1 = 30.1");
    EXPECT_EQ(format("{{name}} is {name}", args), "{name} is Мир");
    EXPECT_EQ(format("}} and {{", args), "} and {");
    EXPECT_EQ(format("{missing} stays", args), "{missing} stays");
    EXPECT_EQ(format("broken {name", args), "broken {name");
    EXPECT_EQ(format("", args), "");
}

TEST(Format, WritesNumbersInTheShortestForm) {
    EXPECT_EQ(fem::l10n::to_text(std::int64_t{-42}), "-42");
    EXPECT_EQ(fem::l10n::to_text(2.5), "2.5");
    EXPECT_EQ(fem::l10n::to_text(1e-7), "1e-07");
    EXPECT_EQ(fem::l10n::to_text(100.0), "100");
    EXPECT_EQ(fem::l10n::to_text(std::string("text")), "text");
}

TEST(Plural, RussianRules) {
    const auto ru = [](std::int64_t n) { return plural_category("ru", n); };
    for (const std::int64_t n : {1, 21, 31, 101, 1001}) {
        EXPECT_EQ(ru(n), PluralCategory::one) << n;
    }
    for (const std::int64_t n : {2, 3, 4, 22, 24, 102, 1004}) {
        EXPECT_EQ(ru(n), PluralCategory::few) << n;
    }
    for (const std::int64_t n : {0, 5, 9, 10, 11, 12, 13, 14, 15, 20, 25, 100, 111, 112, 114}) {
        EXPECT_EQ(ru(n), PluralCategory::many) << n;
    }
    EXPECT_EQ(ru(-1), PluralCategory::one);
    EXPECT_EQ(plural_category("ru", 1.5), PluralCategory::other);
    EXPECT_EQ(plural_category("ru", 2.0), PluralCategory::few)
        << "a whole double is a whole number";
    EXPECT_EQ(plural_category("ru-RU", std::int64_t{5}), PluralCategory::many);
    EXPECT_EQ(plural_category("uk", std::int64_t{3}), PluralCategory::few);
}

TEST(Plural, EnglishRulesAndTheRest) {
    EXPECT_EQ(plural_category("en", std::int64_t{1}), PluralCategory::one);
    EXPECT_EQ(plural_category("en", std::int64_t{0}), PluralCategory::other);
    EXPECT_EQ(plural_category("en", std::int64_t{21}), PluralCategory::other);
    EXPECT_EQ(plural_category("en", 1.0), PluralCategory::one);
    EXPECT_EQ(plural_category("en", 1.5), PluralCategory::other);
    EXPECT_EQ(plural_category("de", std::int64_t{1}), PluralCategory::one);
    EXPECT_EQ(plural_category("en", std::string("1")), PluralCategory::other) << "text: other";
}

TEST(Languages, AreNormalized) {
    EXPECT_EQ(fem::l10n::normalize_language("ru"), "ru");
    EXPECT_EQ(fem::l10n::normalize_language("ru_RU.UTF-8"), "ru-RU");
    EXPECT_EQ(fem::l10n::normalize_language("en_US"), "en-US");
    EXPECT_EQ(fem::l10n::normalize_language("PT-br"), "pt-BR");
    EXPECT_EQ(fem::l10n::normalize_language("sr_RS@latin"), "sr-RS");
    EXPECT_EQ(fem::l10n::normalize_language("zh-hant-tw"), "zh-Hant-TW");
    EXPECT_EQ(fem::l10n::normalize_language("es-419"), "es-419");
    for (const char* bad : {"", "C", "POSIX", "C.UTF-8", "r", "русский", "en-", "en--US", "1234"}) {
        EXPECT_EQ(fem::l10n::normalize_language(bad), "") << bad;
    }
}

TEST(Languages, FallBackToShorterTagsThenEnglish) {
    EXPECT_EQ(fem::l10n::fallback_chain("zh-Hant-TW"),
              (Strings{"zh-Hant-TW", "zh-Hant", "zh", "en"}));
    EXPECT_EQ(fem::l10n::fallback_chain("pt-BR"), (Strings{"pt-BR", "pt", "en"}));
    EXPECT_EQ(fem::l10n::fallback_chain("ru"), (Strings{"ru", "en"}));
    EXPECT_EQ(fem::l10n::fallback_chain("en-GB"), (Strings{"en-GB", "en"}));
    EXPECT_EQ(fem::l10n::fallback_chain("en"), (Strings{"en"}));
}

TEST(Languages, TheSystemLanguageIsATagOrNothing) {
    const std::string language = fem::l10n::system_language();
    EXPECT_EQ(language, fem::l10n::normalize_language(language));
}

class LocalizationTest : public ::testing::Test {
protected:
    LocalizationTest() {
        l10n.add_catalog("test.app", "en", catalog(R"({"run": "Run", "only.en": "Only in English",
                                     "jobs": {"one": "{count} job", "other": "{count} jobs"},
                                     "hello": "Hello, {name}!"})"));
        l10n.add_catalog("test.app", "ru", catalog(R"({"run": "Запустить",
                                     "jobs": {"one": "{count} задание", "few": "{count} задания",
                                              "many": "{count} заданий", "other": "{count} задания"},
                                     "hello": "Привет, {name}!"})"));
        l10n.add_catalog("test.app", "pt", catalog(R"({"run": "Executar"})"));
    }

    Localization l10n;
};

TEST_F(LocalizationTest, StartsInEnglish) {
    EXPECT_EQ(l10n.language(), "en");
    EXPECT_EQ(l10n.text("test.app", "run"), "Run");
}

TEST_F(LocalizationTest, TranslatesWithArguments) {
    l10n.set_language("ru");
    EXPECT_EQ(l10n.text("test.app", "run"), "Запустить");
    EXPECT_EQ(l10n.text("test.app", "hello", {{"name", std::string("Дмитрий")}}),
              "Привет, Дмитрий!");
    EXPECT_EQ(l10n.find("test.app", "hello"), "Привет, {name}!");
}

TEST_F(LocalizationTest, ChoosesPluralFormsByCount) {
    l10n.set_language("ru");
    const auto jobs = [this](std::int64_t n) {
        return l10n.text("test.app", "jobs", {{"count", n}});
    };
    EXPECT_EQ(jobs(1), "1 задание");
    EXPECT_EQ(jobs(3), "3 задания");
    EXPECT_EQ(jobs(5), "5 заданий");
    EXPECT_EQ(jobs(11), "11 заданий");
    EXPECT_EQ(jobs(21), "21 задание");
    EXPECT_EQ(l10n.text("test.app", "jobs", {{"count", 1.5}}), "1.5 задания");
    EXPECT_EQ(l10n.text("test.app", "jobs"), "{count} задания") << "no count: other";
    l10n.set_language("en");
    EXPECT_EQ(jobs(1), "1 job");
    EXPECT_EQ(jobs(5), "5 jobs");
}

TEST_F(LocalizationTest, FallsBackToEnglishThenToTheKey) {
    l10n.set_language("ru");
    EXPECT_EQ(l10n.text("test.app", "only.en"), "Only in English");
    EXPECT_EQ(l10n.text("test.app", "nowhere"), "nowhere");
    EXPECT_EQ(l10n.find("test.app", "nowhere"), std::nullopt);
    EXPECT_EQ(l10n.text("test.unknown", "run"), "run");
    // An English string found for a Russian request follows English plural rules.
    l10n.add_catalog("test.app", "en", catalog(R"({"files": {"one": "{count} file",
                                                         "other": "{count} files"}})"));
    EXPECT_EQ(l10n.text("test.app", "files", {{"count", std::int64_t{1}}}), "1 file");
    EXPECT_EQ(l10n.text("test.app", "files", {{"count", std::int64_t{21}}}), "21 files");
}

TEST_F(LocalizationTest, FallsBackThroughShorterTags) {
    l10n.set_language("pt_BR.UTF-8");
    EXPECT_EQ(l10n.language(), "pt-BR");
    EXPECT_EQ(l10n.text("test.app", "run"), "Executar");
    EXPECT_EQ(l10n.text("test.app", "only.en"), "Only in English");
    l10n.set_language("de");
    EXPECT_EQ(l10n.text("test.app", "run"), "Run");
    l10n.set_language("not a language");
    EXPECT_EQ(l10n.language(), "en");
}

TEST_F(LocalizationTest, ReportsLanguageChanges) {
    Strings changes;
    const fem::Subscription subscription =
        l10n.on_language_changed([&](const std::string& language) { changes.push_back(language); });
    l10n.set_language("ru_RU");
    l10n.set_language("ru-RU");
    l10n.set_language("en");
    EXPECT_EQ(changes, (Strings{"ru-RU", "en"}));
}

TEST_F(LocalizationTest, TranslatesManifestText) {
    l10n.set_language("ru");
    EXPECT_EQ(l10n.manifest_text("test.app", "%run%"), "Запустить");
    EXPECT_EQ(l10n.manifest_text("test.app", "%hello%", {{"name", std::string("всем")}}),
              "Привет, всем!");
    EXPECT_EQ(l10n.manifest_text("test.app", "%nowhere%"), "nowhere");
    // Only the whole text is a key, as in VS Code.
    EXPECT_EQ(l10n.manifest_text("test.app", "Run %run%"), "Run %run%");
    EXPECT_EQ(l10n.manifest_text("test.app", "100%"), "100%");
    EXPECT_EQ(l10n.manifest_text("test.app", "%%"), "%%");
    EXPECT_EQ(l10n.manifest_text("test.app", "Plain title"), "Plain title");
}

TEST_F(LocalizationTest, LanguagePacksTranslateOtherModules) {
    l10n.add_catalog("test.pack", "ru",
                     catalog("{}", R"({"test.app": {"run": "Из пакета", "only.en": "Перевод"},
                                      "fem.core": {"settings.type.integer": "целым"}})"));
    l10n.set_language("ru");
    EXPECT_EQ(l10n.text("test.app", "run"), "Запустить") << "the module's own catalog first";
    EXPECT_EQ(l10n.text("test.app", "only.en"), "Перевод") << "the pack before English";
    EXPECT_EQ(l10n.text("fem.core", "settings.type.integer"), "целым числом")
        << "the core's own catalog first";
    // Packs are tried in the order of their ids.
    l10n.add_catalog("test.another-pack", "ru",
                     catalog("{}", R"({"test.app": {"only.en": "Другой перевод"}})"));
    EXPECT_EQ(l10n.text("test.app", "only.en"), "Другой перевод");
    EXPECT_TRUE(l10n.remove_owner("test.another-pack"));
    EXPECT_EQ(l10n.text("test.app", "only.en"), "Перевод");
    EXPECT_TRUE(l10n.remove_owner("test.pack"));
    EXPECT_FALSE(l10n.remove_owner("test.pack"));
    EXPECT_EQ(l10n.text("test.app", "only.en"), "Only in English");
}

TEST_F(LocalizationTest, ListsLanguagesWithCatalogs) {
    EXPECT_EQ(l10n.languages(), (Strings{"en", "pt", "ru"}));
    l10n.remove_owner("test.app");
    EXPECT_EQ(l10n.languages(), (Strings{"en", "ru"})) << "the catalogs of the core";
}

TEST(LocalizationCore, HasTheCatalogsOfTheCore) {
    Localization l10n;
    EXPECT_TRUE(l10n.has_owner("fem.core"));
    EXPECT_EQ(l10n.text("fem.core", "settings.type.integer"), "an integer");
    l10n.set_language("ru");
    EXPECT_EQ(l10n.text("fem.core", "settings.type.integer"), "целым числом");
    EXPECT_EQ(l10n.text("fem.core", "settings.ignored", {{"count", std::int64_t{3}}}),
              "Не учитываются 3 настройки.");
    EXPECT_EQ(l10n.text("fem.core", "settings.ignored", {{"count", std::int64_t{5}}}),
              "Не учитываются 5 настроек.");
    EXPECT_EQ(l10n.text("fem.core", "settings.ignored", {{"count", std::int64_t{1}}}),
              "Не учитывается 1 настройка.");
}

}  // namespace
