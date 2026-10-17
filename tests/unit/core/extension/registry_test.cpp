#include "fem/core/extension/registry.h"

#include "fem/core/extension/contributions.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manifest.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fem::extension::ContributionRegistry;
using fem::extension::Contributions;
using fem::extension::ExtensionErrc;
using fem::extension::testing::manifest;

/// The contributions of a module written in a test.
std::shared_ptr<const Contributions> contributions(std::string_view id, std::string_view points) {
    const std::string text = "[extension]\nid = \"" + std::string(id) +
                             "\"\nname = \"M\"\nversion = \"1.0.0\"\nengine = \"^1.0\"\n"
                             "kind = \"python\"\nentry = \"m:activate\"\n" +
                             std::string(points);
    return std::make_shared<const Contributions>(manifest(text).contributes);
}

template <class Record>
std::vector<std::string> owners(const std::vector<const Record*>& records) {
    std::vector<std::string> result;
    for (const Record* record : records) {
        result.push_back(record->owner);
    }
    return result;
}

TEST(ContributionRegistry, ListsRecordsByOwnerThenInManifestOrder) {
    ContributionRegistry registry;
    registry.add(contributions("test.b", R"(
[[contributes.commands]]
id = "b.second"
title = "B2"
[[contributes.commands]]
id = "b.first"
title = "B1"
[[contributes.menus]]
location = "tools"
command = "b.first"
[[contributes.submenus]]
id = "b-menu"
title = "B"
)"));
    registry.add(contributions("test.a", R"(
[[contributes.commands]]
id = "a.only"
title = "A"
[[contributes.stages]]
id = "a.stage"
title = "Stage"
slot = "after:results"
)"));
    EXPECT_EQ(registry.owners(), (std::vector<std::string>{"test.a", "test.b"}));
    std::vector<std::string> ids;
    for (const auto* command : registry.commands()) {
        ids.push_back(command->id);
    }
    EXPECT_EQ(ids, (std::vector<std::string>{"a.only", "b.second", "b.first"}));
    EXPECT_EQ(owners(registry.menus()), std::vector<std::string>{"test.b"});
    EXPECT_EQ(owners(registry.submenus()), std::vector<std::string>{"test.b"});
    EXPECT_EQ(owners(registry.stages()), std::vector<std::string>{"test.a"});
    EXPECT_TRUE(registry.solvers().empty());
    ASSERT_NE(registry.find_command("b.first"), nullptr);
    EXPECT_EQ(registry.find_command("b.first")->owner, "test.b");
    EXPECT_EQ(registry.find_command("c.none"), nullptr);
    ASSERT_NE(registry.find("test.a"), nullptr);
    EXPECT_EQ(registry.find("test.a")->commands.size(), 1U);

    EXPECT_TRUE(registry.remove_owner("test.b"));
    EXPECT_FALSE(registry.remove_owner("test.b"));
    EXPECT_FALSE(registry.contains("test.b"));
    EXPECT_TRUE(registry.menus().empty());
    EXPECT_TRUE(registry.submenus().empty());
    EXPECT_EQ(registry.commands().size(), 1U);
    EXPECT_EQ(registry.find_command("b.first"), nullptr);
}

TEST(ContributionRegistry, ApplicationWideIdsAreUniqueAcrossModules) {
    struct Case {
        const char* what;
        const char* points;
    };
    const std::vector<Case> cases = {
        {"command", "[[contributes.commands]]\nid = \"x.run\"\ntitle = \"T\"\n"},
        {"stage", "[[contributes.stages]]\nid = \"x\"\ntitle = \"T\"\nslot = \"after:mesh\"\n"},
        {"file format",
         "[[contributes.fileFormats]]\nid = \"x\"\nextensions = [\".x\"]\n"
         "role = \"mesh-import\"\n"},
        {"solver", "[[contributes.solvers]]\nid = \"x\"\nlabel = \"T\"\n"},
        {"mesher", "[[contributes.meshers]]\nid = \"x\"\nlabel = \"T\"\n"},
        {"activity bar entry",
         "[[contributes.activityBar]]\nid = \"x\"\ntitle = \"T\"\nicon = \"x.svg\"\n"},
        {"bottom panel tab", "[[contributes.bottomPanelTabs]]\nid = \"x\"\ntitle = \"T\"\n"},
        {"submenu", "[[contributes.submenus]]\nid = \"x\"\ntitle = \"T\"\n"},
        {"viewport layer", "[[contributes.viewportLayers]]\nid = \"x\"\ntitle = \"T\"\n"},
        {"result filter", "[[contributes.resultFilters]]\nid = \"x\"\ntitle = \"T\"\n"},
        {"result indicator", "[[contributes.resultIndicators]]\nid = \"x\"\ntitle = \"T\"\n"},
        {"icon", "[[contributes.icons]]\nid = \"x\"\npath = \"x.svg\"\n"},
        {"setting", "[contributes.settings]\n\"x\" = { type = \"boolean\" }\n"},
        {"context key", "[[contributes.contextKeys]]\nname = \"x\"\ntype = \"boolean\"\n"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        ContributionRegistry registry;
        registry.add(contributions("test.first", c.points));
        try {
            registry.add(contributions("test.second", c.points));
            ADD_FAILURE() << "no conflict";
        } catch (const fem::extension::ExtensionError& error) {
            EXPECT_EQ(error.code(), ExtensionErrc::conflict);
            EXPECT_NE(std::string(error.what()).find(std::string(c.what) + " '"), std::string::npos)
                << error.what();
            EXPECT_NE(std::string(error.what()).find("module 'test.first'"), std::string::npos)
                << error.what();
        }
        EXPECT_EQ(registry.owners(), std::vector<std::string>{"test.first"});
    }
}

TEST(ContributionRegistry, ObjectTypesBelongToTheirModule) {
    constexpr std::string_view points = R"(
[[contributes.treeNodeTypes]]
type = "material"
title = "Material"
[[contributes.propertySchemas]]
type = "material"
schema = { type = "object" }
[[contributes.menus]]
location = "tools"
command = "x.run"
)";
    ContributionRegistry registry;
    registry.add(contributions("test.first", points));
    registry.add(contributions("test.second", points));
    EXPECT_EQ(owners(registry.tree_node_types()),
              (std::vector<std::string>{"test.first", "test.second"}));
    EXPECT_EQ(registry.property_schemas().size(), 2U);
    EXPECT_EQ(registry.menus().size(), 2U);
}

TEST(ContributionRegistry, ModuleIsAddedOnce) {
    ContributionRegistry registry;
    registry.add(contributions("test.a", ""));
    EXPECT_EXTENSION_ERROR(registry.add(contributions("test.a", "")), ExtensionErrc::conflict);

    // Copies share the records: a trial copy does not change the original.
    ContributionRegistry trial = registry;
    trial.add(contributions("test.b", "[[contributes.commands]]\nid = \"b.x\"\ntitle = \"B\"\n"));
    EXPECT_EQ(trial.commands().size(), 1U);
    EXPECT_TRUE(registry.commands().empty());
    EXPECT_EQ(trial.find("test.a"), registry.find("test.a"));
}

}  // namespace
