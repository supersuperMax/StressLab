// The order of menu items (`group`) and of stages (slots of the standard stages).

#include "fem/shell/menu_order.h"
#include "fem/shell/stages.h"

#include "shell_test_support.h"
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using fem::shell::MenuGroup;
using fem::shell::parse_menu_group;

TEST(ShellMenuOrder, ReadsGroupsAsVsCodeWritesThem) {
    const auto group = [](std::string_view text) {
        const MenuGroup parsed = parse_menu_group(text);
        return parsed.name + "|" + std::to_string(parsed.order);
    };
    EXPECT_EQ(group("1_run@2"), "1_run|2");
    EXPECT_EQ(group("navigation"), "navigation|0");
    EXPECT_EQ(group("@3"), "|3");
    EXPECT_EQ(group("a@-1"), "a|-1");
    EXPECT_EQ(group(""), "|0");
    // Not a number after '@': all of it is the name.
    EXPECT_EQ(group("mail@home"), "mail@home|0");
    EXPECT_EQ(group("x@1.5"), "x@1.5|0");
    EXPECT_EQ(group("x@"), "x@|0");
}

TEST(ShellMenuOrder, GroupsThenOrderThenTitle) {
    const auto before = [](std::string_view a, const char* title_a, std::string_view b,
                           const char* title_b) {
        return fem::shell::menu_item_before(parse_menu_group(a), QString::fromLatin1(title_a),
                                            parse_menu_group(b), QString::fromLatin1(title_b));
    };
    EXPECT_TRUE(before("1_run", "Z", "2_stop", "A"));
    EXPECT_TRUE(before("1_run@1", "Z", "1_run@2", "A"));
    EXPECT_TRUE(before("1_run@2", "A", "1_run@2", "B"));
    EXPECT_FALSE(before("1_run@2", "B", "1_run@2", "A"));
    // Without a group: last.
    EXPECT_TRUE(before("z_last", "A", "", "A"));
    EXPECT_FALSE(before("", "A", "z_last", "A"));
}

TEST(ShellStages, ModulesFillOnlyTheSlotsOfStandardStages) {
    fem::shell::testing::Application app;
    app.add("test.b", R"(
[[contributes.stages]]
id = "b.before-solve"
title = "B"
slot = "before:solve"
[[contributes.stages]]
id = "b.after-results"
title = "B2"
slot = "after:results"
)");
    app.add("test.a", R"(
[[contributes.stages]]
id = "a.before-solve"
title = "A"
slot = "before:solve"
[[contributes.stages]]
id = "a.inside"
title = "Inside"
slot = "after:b.before-solve"
)");
    std::vector<const fem::extension::StageContribution*> rejected;
    std::vector<std::string> ids;
    for (const fem::shell::StageEntry& stage :
         fem::shell::stage_order(app.manager.registry(), &rejected)) {
        ids.push_back(stage.id);
        EXPECT_EQ(stage.contribution == nullptr, fem::shell::is_standard_stage(stage.id));
    }
    // One slot keeps the order of the registry: by module, then as in the manifest.
    EXPECT_EQ(ids,
              (std::vector<std::string>{"geometry", "mesh", "materials", "steps", "a.before-solve",
                                        "b.before-solve", "solve", "results", "b.after-results"}));
    ASSERT_EQ(rejected.size(), 1U);
    EXPECT_EQ(rejected[0]->id, "a.inside");
    EXPECT_EQ(fem::shell::stage_title(app.localization, app.manager.registry(), "solve"),
              QStringLiteral("Solve"));
    EXPECT_EQ(fem::shell::stage_title(app.localization, app.manager.registry(), "a.before-solve"),
              QStringLiteral("A"));
}

TEST(ShellStages, TheStageKeyBelongsToTheShell) {
    fem::context::ContextKeys keys;
    fem::shell::declare_stage_key(keys);
    fem::shell::declare_stage_key(keys);  // Twice is fine.
    ASSERT_NE(keys.find("stage"), nullptr);
    EXPECT_EQ(keys.find("stage")->owner, "fem.shell");
    EXPECT_EQ(keys.value("stage"), fem::context::ContextValue("geometry"));
}

}  // namespace
