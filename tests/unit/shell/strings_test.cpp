// The catalogs of the shell (shell/l10n) are complete like those of the core: every language has
// every string of English with the same placeholders, and Russian is there from the first day.

#include "fem/shell/strings.h"

#include "fem/core/l10n/catalog.h"
#include "fem/core/l10n/localization.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>

namespace {

namespace fs = std::filesystem;
using fem::l10n::Catalog;
using fem::l10n::Message;

std::set<std::string, std::less<>> placeholders_of(const Message& message) {
    std::set<std::string, std::less<>> names;
    for (const auto& [category, text] : message.forms) {
        names.merge(fem::l10n::placeholders(text));
    }
    return names;
}

TEST(ShellStrings, EveryLanguageHasTheStringsOfEnglish) {
    const fs::path folder(FEM_SHELL_L10N_DIR);
    const Catalog english = fem::l10n::load_catalog(folder / "en.json");
    int languages = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        const std::string language = entry.path().stem().string();
        if (language == "en") {
            continue;
        }
        ++languages;
        const Catalog catalog = fem::l10n::load_catalog(entry.path());
        for (const auto& [key, message] : english.strings) {
            const auto found = catalog.strings.find(key);
            if (found == catalog.strings.end()) {
                ADD_FAILURE() << language << " has no '" << key << "'";
                continue;
            }
            EXPECT_EQ(placeholders_of(found->second), placeholders_of(message))
                << language << ": placeholders of '" << key << "'";
        }
        for (const auto& [key, message] : catalog.strings) {
            EXPECT_TRUE(english.strings.contains(key)) << language << " has extra '" << key << "'";
        }
    }
    EXPECT_GE(languages, 1);
}

TEST(ShellStrings, FollowTheLanguageOfTheLocalization) {
    fem::l10n::Localization localization;
    fem::shell::add_shell_catalogs(localization);
    EXPECT_TRUE(localization.has_owner(fem::shell::strings_owner));
    EXPECT_EQ(fem::shell::text(localization, "zone.modelTree"), "Model Tree");
    localization.set_language("ru-RU");
    EXPECT_EQ(fem::shell::text(localization, "zone.modelTree"), "Дерево модели");
    EXPECT_EQ(fem::shell::text(localization, "workspace.deleteNamed", {{"name", "Сетка"}}),
              "Удалить рабочее пространство «Сетка»");
    // A language without a catalog falls back to English.
    localization.set_language("de");
    EXPECT_EQ(fem::shell::text(localization, "zone.problems"), "Problems");
}

}  // namespace
