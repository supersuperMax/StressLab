#pragma once

#include <QString>
#include <string>
#include <string_view>

// The order of items in menus, context menus and the context toolbar: the `group` of a manifest
// entry, written as in VS Code, "<group>@<order>" (docs/adr/0013-shell-contributions.md).
namespace fem::shell {

/// The group of the context toolbar whose commands always go to its "⋯" menu.
inline constexpr std::string_view overflow_group = "more";

/// A parsed `group`.
struct MenuGroup {
    /// Items of one group stand together, groups in the order of their names (text, so "1_run"
    /// comes before "2_stop"), separated by a line; items without a group come last.
    std::string name;
    /// Order within the group (an integer), smaller first; 0 if not given.
    int order = 0;
};

/// "1_run@2" → {"1_run", 2}; "navigation" → {"navigation", 0}; "@3" → {"", 3}. Text after '@'
/// that is not a number belongs to the name.
[[nodiscard]] MenuGroup parse_menu_group(std::string_view group);

/// True if an item of group `a` titled `title_a` comes before an item of `b` titled `title_b`:
/// by group, then by order, then by title.
[[nodiscard]] bool menu_item_before(const MenuGroup& a, const QString& title_a, const MenuGroup& b,
                                    const QString& title_b);

}  // namespace fem::shell
