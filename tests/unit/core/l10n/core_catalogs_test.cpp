// The catalogs of the core (core/l10n) are complete: every language has every string of English,
// with the same placeholders and nothing English lacks (task 1.7).

#include "fem/core/l10n/catalog.h"
#include "fem/core/settings/error.h"
#include "fem/core/settings/settings.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>

namespace {

namespace fs = std::filesystem;
using fem::l10n::Catalog;
using fem::l10n::Message;

fs::path catalogs_folder() {
    return fs::path(FEM_CORE_L10N_DIR);
}

std::set<std::string, std::less<>> placeholders_of(const Message& message) {
    std::set<std::string, std::less<>> names;
    for (const auto& [category, text] : message.forms) {
        names.merge(fem::l10n::placeholders(text));
    }
    return names;
}

TEST(CoreCatalogs, EveryLanguageHasTheStringsOfEnglish) {
    const Catalog english = fem::l10n::load_catalog(catalogs_folder() / "en.json");
    int languages = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(catalogs_folder())) {
        const std::string language = entry.path().stem().string();
        if (language == "en") {
            continue;
        }
        ++languages;
        const Catalog catalog = fem::l10n::load_catalog(entry.path());
        EXPECT_TRUE(catalog.modules.empty()) << language;
        for (const auto& [key, message] : english.strings) {
            const auto found = catalog.strings.find(key);
            if (found == catalog.strings.end()) {
                ADD_FAILURE() << language << " has no '" << key << "'";
                continue;
            }
            EXPECT_EQ(placeholders_of(found->second), placeholders_of(message))
                << language << ": placeholders of '" << key << "'";
            EXPECT_EQ(found->second.is_plural(), message.is_plural()) << language << ": " << key;
        }
        for (const auto& [key, message] : catalog.strings) {
            EXPECT_TRUE(english.strings.contains(key)) << language << " has extra '" << key << "'";
        }
    }
    EXPECT_GE(languages, 1) << "Russian is there from the first day";
}

TEST(CoreCatalogs, HaveTheTextsTheCoreAsksFor) {
    const Catalog english = fem::l10n::load_catalog(catalogs_folder() / "en.json");
    using fem::settings::SettingsErrc;
    for (const SettingsErrc code :
         {SettingsErrc::unknown_setting, SettingsErrc::wrong_type, SettingsErrc::not_in_enum,
          SettingsErrc::below_minimum, SettingsErrc::above_maximum, SettingsErrc::wrong_scope,
          SettingsErrc::duplicate_setting, SettingsErrc::invalid_file, SettingsErrc::io}) {
        const std::string key = "settings.error." + std::string(to_string(code));
        EXPECT_TRUE(english.strings.contains(key)) << key;
    }
    for (const char* type : {"boolean", "integer", "number", "string", "path"}) {
        EXPECT_TRUE(english.strings.contains(std::string("settings.type.") + type)) << type;
    }
    for (const fem::settings::Schema& schema : fem::settings::core_schemas()) {
        const std::string& description = schema.description;
        ASSERT_GE(description.size(), 2U);
        const std::string key = description.substr(1, description.size() - 2);
        EXPECT_TRUE(english.strings.contains(key)) << key;
    }
}

}  // namespace
