#include "fem/shell/history_graph.h"

#include <algorithm>
#include <map>
#include <set>
#include <string_view>

namespace fem::shell {

namespace {

using Index = std::map<std::string, std::size_t, std::less<>>;

/// Gives `lane` to `start` and to its first parents until one has a lane; false if `start` had
/// one already.
bool claim_line(const std::vector<GraphVersion>& versions, const Index& index, std::size_t start,
                int lane, std::vector<int>& lanes) {
    bool claimed = false;
    std::size_t at = start;
    while (lanes[at] < 0) {
        lanes[at] = lane;
        claimed = true;
        if (versions[at].parents.empty()) {
            break;
        }
        const auto parent = index.find(versions[at].parents.front());
        if (parent == index.end()) {
            break;
        }
        at = parent->second;
    }
    return claimed;
}

/// The shown versions an edge to `id` ends at: `id` itself if it is shown, otherwise its nearest
/// shown ancestors.
void shown_ancestors(const std::vector<GraphVersion>& versions, const Index& index,
                     const std::vector<bool>& visible, std::string_view id,
                     std::set<std::size_t>& seen, std::vector<std::size_t>& out) {
    const auto found = index.find(id);
    if (found == index.end() || !seen.insert(found->second).second) {
        return;
    }
    if (visible[found->second]) {
        out.push_back(found->second);
        return;
    }
    for (const std::string& parent : versions[found->second].parents) {
        shown_ancestors(versions, index, visible, parent, seen, out);
    }
}

void add_line(GraphRow& row, GraphLine line) {
    if (std::find(row.lines.begin(), row.lines.end(), line) == row.lines.end()) {
        row.lines.push_back(line);
    }
}

}  // namespace

GraphLayout layout_graph(const std::vector<GraphVersion>& versions,
                         const std::vector<GraphHead>& heads,
                         const std::function<bool(std::size_t)>& shown) {
    const std::size_t count = versions.size();
    Index index;
    for (std::size_t i = 0; i < count; ++i) {
        index.emplace(versions[i].id, i);
    }

    // A lane per variant, in the order of the heads, then one per line no variant reaches.
    std::vector<int> lanes(count, -1);
    std::vector<std::string> lane_variants;
    for (const GraphHead& head : heads) {
        const auto found = index.find(head.version);
        const int lane = static_cast<int>(lane_variants.size());
        if (found != index.end() && claim_line(versions, index, found->second, lane, lanes)) {
            lane_variants.push_back(head.name);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const int lane = static_cast<int>(lane_variants.size());
        if (claim_line(versions, index, i, lane, lanes)) {
            lane_variants.emplace_back();
        }
    }

    std::vector<bool> visible(count);
    for (std::size_t i = 0; i < count; ++i) {
        visible[i] = !shown || shown(i);
    }

    // Lanes without a shown version go; the others keep their order.
    std::vector<int> renumbered(lane_variants.size(), -1);
    GraphLayout layout;
    for (std::size_t i = 0; i < count; ++i) {
        if (visible[i]) {
            renumbered[static_cast<std::size_t>(lanes[i])] = 0;
        }
    }
    for (std::size_t lane = 0; lane < renumbered.size(); ++lane) {
        if (renumbered[lane] == 0) {
            renumbered[lane] = layout.lanes++;
            layout.lane_variants.push_back(lane_variants[lane]);
        }
    }

    std::vector<std::size_t> row_of(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        if (visible[i]) {
            row_of[i] = layout.rows.size();
            layout.rows.push_back({.version = i,
                                   .lane = renumbered[static_cast<std::size_t>(lanes[i])],
                                   .lines = {}});
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (!visible[i]) {
            continue;
        }
        const std::size_t row = row_of[i];
        const int lane = layout.rows[row].lane;
        std::set<std::size_t> seen;
        std::vector<std::size_t> parents;
        for (const std::string& parent : versions[i].parents) {
            shown_ancestors(versions, index, visible, parent, seen, parents);
        }
        if (parents.empty() && !versions[i].parents.empty()) {
            // Its parents are not listed: the line goes on below it.
            add_line(layout.rows[row], {lane, GraphPoint::middle, lane, GraphPoint::bottom, lane});
        }
        for (const std::size_t parent : parents) {
            const std::size_t parent_row = row_of[parent];
            if (parent_row <= row) {
                continue;  // Not listed after its child: nothing sensible to draw.
            }
            add_line(layout.rows[row], {lane, GraphPoint::middle, lane, GraphPoint::bottom, lane});
            for (std::size_t between = row + 1; between < parent_row; ++between) {
                add_line(layout.rows[between],
                         {lane, GraphPoint::top, lane, GraphPoint::bottom, lane});
            }
            add_line(layout.rows[parent_row], {lane, GraphPoint::top, layout.rows[parent_row].lane,
                                               GraphPoint::middle, lane});
        }
    }
    return layout;
}

}  // namespace fem::shell
