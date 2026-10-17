// The layout of the graph of versions (task 2.6): a lane per variant, lines between a version and
// the versions it was made on, filters that keep the lines whole.

#include "fem/shell/history_graph.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using fem::shell::GraphHead;
using fem::shell::GraphLayout;
using fem::shell::GraphLine;
using fem::shell::GraphPoint;
using fem::shell::GraphVersion;
using fem::shell::layout_graph;

constexpr GraphPoint top = GraphPoint::top;
constexpr GraphPoint middle = GraphPoint::middle;
constexpr GraphPoint bottom = GraphPoint::bottom;

/// The ids of the rows.
std::vector<std::string> ids(const GraphLayout& layout, const std::vector<GraphVersion>& versions) {
    std::vector<std::string> out;
    for (const auto& row : layout.rows) {
        out.push_back(versions[row.version].id);
    }
    return out;
}

/// Main: m1 ← m2 ← m3; variant "B" from m1: b1 ← b2. Newest first: m3, b2, m2, b1, m1.
std::vector<GraphVersion> wing() {
    return {{"m3", {"m2"}}, {"b2", {"b1"}}, {"m2", {"m1"}}, {"b1", {"m1"}}, {"m1", {}}};
}

const std::vector<GraphHead> wing_heads{{"main", "m3"}, {"B", "b2"}};

}  // namespace

TEST(HistoryGraph, OneVariantIsOneLane) {
    const std::vector<GraphVersion> versions{{"c", {"b"}}, {"b", {"a"}}, {"a", {}}};
    const GraphLayout layout = layout_graph(versions, {{"main", "c"}});
    EXPECT_EQ(layout.lanes, 1);
    EXPECT_EQ(layout.lane_variants, std::vector<std::string>{"main"});
    ASSERT_EQ(layout.rows.size(), 3U);
    EXPECT_EQ(layout.rows[0].lines, (std::vector<GraphLine>{{0, middle, 0, bottom, 0}}));
    EXPECT_EQ(layout.rows[1].lines,
              (std::vector<GraphLine>{{0, top, 0, middle, 0}, {0, middle, 0, bottom, 0}}));
    EXPECT_EQ(layout.rows[2].lines, (std::vector<GraphLine>{{0, top, 0, middle, 0}}));
}

TEST(HistoryGraph, EachVariantKeepsItsLaneAndColour) {
    const std::vector<GraphVersion> versions = wing();
    const GraphLayout layout = layout_graph(versions, wing_heads);
    EXPECT_EQ(layout.lanes, 2);
    EXPECT_EQ(layout.lane_variants, (std::vector<std::string>{"main", "B"}));
    ASSERT_EQ(ids(layout, versions), (std::vector<std::string>{"m3", "b2", "m2", "b1", "m1"}));
    // The shared start belongs to the main variant.
    EXPECT_EQ(layout.rows[0].lane, 0);
    EXPECT_EQ(layout.rows[1].lane, 1);
    EXPECT_EQ(layout.rows[2].lane, 0);
    EXPECT_EQ(layout.rows[3].lane, 1);
    EXPECT_EQ(layout.rows[4].lane, 0);
    // b1 → m1 leaves the lane of B in the row of m1, in the colour of B.
    const std::vector<GraphLine>& last = layout.rows[4].lines;
    EXPECT_NE(std::find(last.begin(), last.end(), GraphLine{1, top, 0, middle, 1}), last.end());
    EXPECT_NE(std::find(last.begin(), last.end(), GraphLine{0, top, 0, middle, 0}), last.end());
    // m3 → m2 passes the row of b2 in the lane of the main variant.
    const std::vector<GraphLine>& b2 = layout.rows[1].lines;
    EXPECT_NE(std::find(b2.begin(), b2.end(), GraphLine{0, top, 0, bottom, 0}), b2.end());
}

TEST(HistoryGraph, AVariantWithoutVersionsOfItsOwnHasNoLane) {
    const std::vector<GraphVersion> versions = wing();
    const GraphLayout layout = layout_graph(versions, {{"main", "m3"}, {"A", "m2"}, {"B", "b2"}});
    EXPECT_EQ(layout.lane_variants, (std::vector<std::string>{"main", "B"}));
}

TEST(HistoryGraph, VersionsNoVariantReachesGetLanesOfTheirOwn) {
    // Changes put aside on m2.
    std::vector<GraphVersion> versions = wing();
    versions.insert(versions.begin(), {"s", {"m2"}});
    const GraphLayout layout = layout_graph(versions, wing_heads);
    EXPECT_EQ(layout.lanes, 3);
    EXPECT_EQ(layout.lane_variants, (std::vector<std::string>{"main", "B", ""}));
    EXPECT_EQ(layout.rows[0].lane, 2);
}

TEST(HistoryGraph, HiddenVersionsKeepTheLinesWhole) {
    const std::vector<GraphVersion> versions = wing();
    // Without m2 and b1: m3 goes to m1, and so does b2.
    const GraphLayout layout = layout_graph(versions, wing_heads, [&](std::size_t index) {
        return versions[index].id != "m2" && versions[index].id != "b1";
    });
    ASSERT_EQ(ids(layout, versions), (std::vector<std::string>{"m3", "b2", "m1"}));
    const std::vector<GraphLine>& last = layout.rows[2].lines;
    EXPECT_NE(std::find(last.begin(), last.end(), GraphLine{0, top, 0, middle, 0}), last.end());
    EXPECT_NE(std::find(last.begin(), last.end(), GraphLine{1, top, 0, middle, 1}), last.end());
    // A lane without a shown version goes.
    const GraphLayout main_only = layout_graph(
        versions, wing_heads, [&](std::size_t index) { return versions[index].id.front() == 'm'; });
    EXPECT_EQ(main_only.lanes, 1);
    EXPECT_EQ(main_only.lane_variants, std::vector<std::string>{"main"});
}

TEST(HistoryGraph, AParentThatIsNotListedLeavesALineDown) {
    // The log was cut short: b's parent is not in it.
    const std::vector<GraphVersion> versions{{"c", {"b"}}, {"b", {"a"}}};
    const GraphLayout layout = layout_graph(versions, {{"main", "c"}});
    EXPECT_EQ(layout.rows[1].lines,
              (std::vector<GraphLine>{{0, top, 0, middle, 0}, {0, middle, 0, bottom, 0}}));
    EXPECT_TRUE(layout_graph({}, {}).rows.empty());
}
