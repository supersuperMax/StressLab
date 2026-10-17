#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The layout of the graph of versions in the panel "History" (task 2.6, docs/interface-design.md
// §12): a row per version, newest first, and a lane (column) per variant, so that each variant
// keeps its line and its colour:
//
//   Main        ●──●──●────●  "Brought the skin over"
//                   └──┐
//   Variant B          ●──●   "Composite skin"
//
// Nothing here draws: the panel paints the lines a row gives. Without Qt, for tests.
namespace fem::shell {

/// A version as the graph needs it.
struct GraphVersion {
    std::string id;
    /// Ids; the first is the version it was made on.
    std::vector<std::string> parents;
};

/// Where a line of a row starts or ends, from the top of the row down.
enum class GraphPoint : std::uint8_t { top, middle, bottom };

/// A straight line in a row, from a point of one lane to a point of another (or the same) lane.
struct GraphLine {
    int from_lane = 0;
    GraphPoint from = GraphPoint::top;
    int to_lane = 0;
    GraphPoint to = GraphPoint::bottom;
    /// The lane whose colour the line has: the lane of the later version of the edge.
    int color_lane = 0;

    friend bool operator==(const GraphLine&, const GraphLine&) = default;
};

/// A row of the graph: a version shown.
struct GraphRow {
    /// The index of the version in the list given to `layout_graph`.
    std::size_t version = 0;
    /// The lane of its node.
    int lane = 0;
    std::vector<GraphLine> lines;
};

/// A variant whose line the graph draws.
struct GraphHead {
    std::string name;
    std::string version;  ///< Its last version.
};

/// The graph.
struct GraphLayout {
    std::vector<GraphRow> rows;
    /// The number of lanes in use.
    int lanes = 0;
    /// The variant each lane belongs to; empty for a lane of versions no variant reaches (changes
    /// put aside, a version viewed on its own).
    std::vector<std::string> lane_variants;
};

/// Lays out `versions` (children before their parents, as the history lists them). `heads` are
/// the variants in the order of their lanes (the main variant first): a version takes the lane of
/// the first variant whose line of first parents reaches it; versions no variant reaches get
/// lanes of their own after them. Only versions `shown` accepts get rows (all if it is empty);
/// an edge to a hidden version goes on to its nearest shown ancestors, so the lines stay whole.
/// Lanes without a shown version are dropped.
[[nodiscard]] GraphLayout layout_graph(const std::vector<GraphVersion>& versions,
                                       const std::vector<GraphHead>& heads,
                                       const std::function<bool(std::size_t)>& shown = {});

}  // namespace fem::shell
