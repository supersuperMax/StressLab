// The demonstration module extensions/fem.demo (task 1.15) as the application installs it: its
// manifest, the form of its notes, its catalogs and its setting. What it does in the window and in
// headless runs is tested by tests/unit/shell/demo_acceptance_test.cpp and
// python/tests/test_demo.py.

#include "fem/core/bindings.h"
#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/extension/contributions.h"
#include "fem/core/extension/manager.h"
#include "fem/core/form/form.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/document.h"
#include "fem/core/settings/settings.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::extension::ExtensionManager;
using fem::extension::ExtensionSource;
using fem::extension::StageContribution;

const fs::path folder = fs::path(FEM_BUILTIN_EXTENSIONS_DIR) / "fem.demo";

std::string read(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// The keys of a string catalog of the module.
std::set<std::string> catalog_keys(std::string_view language) {
    const nlohmann::json catalog =
        nlohmann::json::parse(read(folder / "l10n" / (std::string(language) + ".json")));
    std::set<std::string> keys;
    for (const auto& [key, value] : catalog.at("strings").items()) {
        keys.insert(key);
    }
    return keys;
}

/// The module installed from the folder the application ships and enabled, on the platform version
/// of this build.
class DemoModule : public ::testing::Test {
protected:
    DemoModule() {
        (void)manager.install(folder, ExtensionSource::builtin);
        (void)manager.enable("fem.demo");
    }

    [[nodiscard]] const fem::extension::Contributions& contributions() const {
        return *manager.registry().find("fem.demo");
    }

    fem::model::Document document;
    fem::command::CommandBus bus{document};
    fem::context::ContextKeys keys;
    ExtensionManager manager{document, bus, keys};
};

TEST_F(DemoModule, ContributesOneOfEachKindTheTaskNames) {
    const auto& demo = contributions();
    ASSERT_EQ(demo.commands.size(), 1U);
    EXPECT_EQ(demo.commands[0].id, "demo.add_note");
    ASSERT_EQ(demo.commands[0].args.size(), 1U);
    EXPECT_EQ(demo.commands[0].args[0].name, "text");
    EXPECT_TRUE(bus.find_command("demo.add_note") != nullptr);

    ASSERT_EQ(demo.context_toolbar.size(), 1U);
    EXPECT_EQ(demo.context_toolbar[0].command, "demo.add_note");
    ASSERT_TRUE(demo.context_toolbar[0].when.has_value());
    EXPECT_EQ(demo.context_toolbar[0].when->to_string(), "stage == 'demo.review'");

    ASSERT_EQ(demo.stages.size(), 1U);
    EXPECT_EQ(demo.stages[0].id, "demo.review");
    EXPECT_EQ(demo.stages[0].placement, StageContribution::Placement::after);
    EXPECT_EQ(demo.stages[0].anchor, "results");

    ASSERT_EQ(demo.bottom_panel_tabs.size(), 1U);
    EXPECT_EQ(demo.bottom_panel_tabs[0].id, "demo.notes");

    ASSERT_EQ(demo.tree_node_types.size(), 1U);
    EXPECT_EQ(demo.tree_node_types[0].type, "demo_note");
    ASSERT_EQ(demo.property_schemas.size(), 1U);
    EXPECT_EQ(demo.property_schemas[0].type, "demo_note");

    ASSERT_EQ(demo.settings.size(), 1U);
    EXPECT_EQ(demo.settings[0].key, "demo.defaultText");
}

TEST_F(DemoModule, FormOfANoteHasNoMistakes) {
    std::vector<fem::form::SchemaError> errors;
    const fem::form::FormSpec form =
        fem::form::parse(contributions().property_schemas[0].schema, &errors);
    for (const auto& error : errors) {
        ADD_FAILURE() << error.pointer << ": " << error.message;
    }
    std::vector<std::string> fields;
    for (const fem::form::Field& field : form.fields) {
        fields.push_back(field.key);
    }
    EXPECT_EQ(fields, (std::vector<std::string>{"text", "priority", "offset"}));
}

TEST_F(DemoModule, EveryTextOfTheManifestIsTranslatedToEnglishAndRussian) {
    const std::set<std::string> english = catalog_keys("en");
    EXPECT_EQ(catalog_keys("ru"), english);

    const std::string manifest = read(folder / "stresslab-extension.toml");
    const std::regex reference("%([A-Za-z0-9_.-]+)%");
    std::set<std::string> referenced;
    for (std::sregex_iterator it(manifest.begin(), manifest.end(), reference), end; it != end;
         ++it) {
        referenced.insert((*it)[1].str());
    }
    EXPECT_FALSE(referenced.empty());
    for (const std::string& key : referenced) {
        EXPECT_TRUE(english.contains(key)) << "%" << key << "% has no text";
    }
    for (const std::string& key : english) {
        EXPECT_TRUE(referenced.contains(key)) << key << " is not used";
    }

    fem::l10n::Localization localization;
    const fem::Subscription catalogs = fem::bind_catalogs(manager, localization);
    localization.set_language("ru");
    EXPECT_EQ(localization.manifest_text("fem.demo", "%stage.review%"), "Проверка");
    localization.set_language("en");
    EXPECT_EQ(localization.manifest_text("fem.demo", "%stage.review%"), "Review");
}

TEST_F(DemoModule, ItsSettingHasADefaultForTheCode) {
    fem::settings::Settings settings;
    const fem::Subscription binding = fem::bind_settings(manager, settings);
    EXPECT_EQ(settings.value("demo.defaultText"), "TODO");
}

}  // namespace
