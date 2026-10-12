#include "fem/core/model/element_type.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

using namespace fem::model;

namespace {

using Point = std::array<double, 3>;

// Corner coordinates of the reference elements in VTK's parametric space.
std::vector<Point> reference_corners(ElementType linear) {
    switch (linear) {
        case ElementType::tet4:
            return {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        case ElementType::pyramid5:
            return {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0.5, 0.5, 1}};
        case ElementType::wedge6:
            return {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 1}, {1, 0, 1}};
        case ElementType::hex8:
            return {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                    {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
        default:
            return {};
    }
}

std::vector<ElementType> linear_solids() {
    std::vector<ElementType> result;
    for (const ElementType type : all_element_types) {
        const ElementTypeInfo& info = element_type_info(type);
        if (info.dimension == 3 && info.linear == type) {
            result.push_back(type);
        }
    }
    return result;
}

Point centroid(const std::vector<Point>& points) {
    Point sum{};
    for (const Point& p : points) {
        for (std::size_t i = 0; i < 3; ++i) {
            sum[i] += p[i] / static_cast<double>(points.size());
        }
    }
    return sum;
}

// Newell's method: normal of a planar polygon, oriented by the right-hand rule.
Point polygon_normal(const std::vector<Point>& polygon) {
    Point n{};
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point& a = polygon[i];
        const Point& b = polygon[(i + 1) % polygon.size()];
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    return n;
}

std::vector<Edge> to_vector(std::span<const Edge> edges) {
    return {edges.begin(), edges.end()};
}

}  // namespace

TEST(ElementType, EveryTypeIsListedOnceInEnumOrder) {
    ASSERT_EQ(all_element_types.size(), 15U);
    for (std::size_t i = 0; i < all_element_types.size(); ++i) {
        EXPECT_EQ(static_cast<std::size_t>(all_element_types[i]), i);
        EXPECT_EQ(element_type_info(all_element_types[i]).type, all_element_types[i]);
    }
}

TEST(ElementType, NamesAreUniqueAndParseBack) {
    std::set<std::string_view> names;
    for (const ElementType type : all_element_types) {
        const std::string_view name = to_string(type);
        EXPECT_TRUE(names.insert(name).second) << name;
        EXPECT_EQ(parse_element_type(name), type);
    }
    EXPECT_EQ(parse_element_type("TET10"), std::nullopt);
    EXPECT_EQ(parse_element_type("tet"), std::nullopt);
    EXPECT_EQ(parse_element_type(""), std::nullopt);
}

TEST(ElementType, NodeCounts) {
    const std::map<ElementType, int> expected = {
        {ElementType::vertex, 1},   {ElementType::line2, 2},      {ElementType::line3, 3},
        {ElementType::tri3, 3},     {ElementType::tri6, 6},       {ElementType::quad4, 4},
        {ElementType::quad8, 8},    {ElementType::tet4, 4},       {ElementType::tet10, 10},
        {ElementType::pyramid5, 5}, {ElementType::pyramid13, 13}, {ElementType::wedge6, 6},
        {ElementType::wedge15, 15}, {ElementType::hex8, 8},       {ElementType::hex20, 20},
    };
    for (const auto& [type, count] : expected) {
        EXPECT_EQ(nodes_per_element(type), count) << to_string(type);
    }
}

TEST(ElementType, QuadraticTypesAddOneNodePerEdgeOfTheirLinearShape) {
    for (const ElementType type : all_element_types) {
        const ElementTypeInfo& info = element_type_info(type);
        const ElementTypeInfo& linear = element_type_info(info.linear);
        SCOPED_TRACE(info.name);

        EXPECT_EQ(linear.linear, linear.type);
        EXPECT_EQ(linear.node_count, linear.corner_count);
        EXPECT_EQ(info.corner_count, linear.corner_count);
        EXPECT_EQ(info.dimension, linear.dimension);
        EXPECT_EQ(to_vector(info.edges), to_vector(linear.edges));
        EXPECT_EQ(info.sides.size(), linear.sides.size());
        if (info.linear != type) {
            EXPECT_EQ(info.node_count, info.corner_count + static_cast<int>(info.edges.size()));
        }
    }
}

// Pins the neutral (VTK) order of mid-edge nodes, where formats differ the most.
TEST(ElementType, MidEdgeNodesFollowVtkOrder) {
    const auto mid_edge = [](ElementType type, int node) {
        const ElementTypeInfo& info = element_type_info(type);
        return info.edges[static_cast<std::size_t>(node - info.corner_count)];
    };
    EXPECT_EQ(mid_edge(ElementType::line3, 2), (Edge{0, 1}));
    EXPECT_EQ(mid_edge(ElementType::tri6, 5), (Edge{2, 0}));
    EXPECT_EQ(mid_edge(ElementType::quad8, 7), (Edge{3, 0}));
    EXPECT_EQ(mid_edge(ElementType::tet10, 6), (Edge{2, 0}));
    EXPECT_EQ(mid_edge(ElementType::tet10, 7), (Edge{0, 3}));
    EXPECT_EQ(mid_edge(ElementType::tet10, 9), (Edge{2, 3}));
    EXPECT_EQ(mid_edge(ElementType::pyramid13, 9), (Edge{0, 4}));
    EXPECT_EQ(mid_edge(ElementType::pyramid13, 12), (Edge{3, 4}));
    EXPECT_EQ(mid_edge(ElementType::wedge15, 11), (Edge{5, 3}));
    EXPECT_EQ(mid_edge(ElementType::wedge15, 12), (Edge{0, 3}));
    EXPECT_EQ(mid_edge(ElementType::hex20, 15), (Edge{7, 4}));
    EXPECT_EQ(mid_edge(ElementType::hex20, 18), (Edge{2, 6}));
    EXPECT_EQ(mid_edge(ElementType::hex20, 19), (Edge{3, 7}));
}

TEST(ElementType, EdgesAndSidesReferToDistinctCornerNodes) {
    for (const ElementType type : all_element_types) {
        const ElementTypeInfo& info = element_type_info(type);
        SCOPED_TRACE(info.name);

        std::set<std::pair<int, int>> undirected;
        for (const Edge& edge : info.edges) {
            EXPECT_LT(edge[0], info.corner_count);
            EXPECT_LT(edge[1], info.corner_count);
            EXPECT_NE(edge[0], edge[1]);
            EXPECT_TRUE(
                undirected.emplace(std::min(edge[0], edge[1]), std::max(edge[0], edge[1])).second);
        }
        for (const Side& side : info.sides) {
            const std::span<const LocalNode> corners = side.corners();
            EXPECT_GE(corners.size(), 1U);
            const std::set<LocalNode> unique(corners.begin(), corners.end());
            EXPECT_EQ(unique.size(), corners.size());
            for (const LocalNode node : corners) {
                EXPECT_LT(node, info.corner_count);
            }
        }
    }
}

TEST(ElementType, SidesOfLowerDimensionalShapes) {
    EXPECT_TRUE(element_type_info(ElementType::vertex).sides.empty());
    EXPECT_TRUE(element_type_info(ElementType::vertex).edges.empty());

    // Lines: the two end points.
    const auto line_sides = element_type_info(ElementType::line2).sides;
    ASSERT_EQ(line_sides.size(), 2U);
    EXPECT_EQ(line_sides[0].size, 1);
    EXPECT_EQ(line_sides[0].nodes[0], 0);
    EXPECT_EQ(line_sides[1].nodes[0], 1);

    // Surfaces: the edges, in edge order.
    for (const ElementType type : {ElementType::tri3, ElementType::quad4}) {
        const ElementTypeInfo& info = element_type_info(type);
        ASSERT_EQ(info.sides.size(), info.edges.size());
        for (std::size_t i = 0; i < info.sides.size(); ++i) {
            ASSERT_EQ(info.sides[i].size, 2);
            EXPECT_EQ(info.sides[i].nodes[0], info.edges[i][0]);
            EXPECT_EQ(info.sides[i].nodes[1], info.edges[i][1]);
        }
    }
}

TEST(ElementType, SideCountsOfSolids) {
    const auto count = [](ElementType type, std::size_t corners) {
        const auto sides = element_type_info(type).sides;
        return std::count_if(sides.begin(), sides.end(),
                             [corners](const Side& side) { return side.size == corners; });
    };
    EXPECT_EQ(count(ElementType::tet4, 3), 4);
    EXPECT_EQ(count(ElementType::pyramid5, 4), 1);
    EXPECT_EQ(count(ElementType::pyramid5, 3), 4);
    EXPECT_EQ(count(ElementType::wedge6, 3), 2);
    EXPECT_EQ(count(ElementType::wedge6, 4), 3);
    EXPECT_EQ(count(ElementType::hex8, 4), 6);
}

// Every edge of a solid borders exactly two sides, once in each direction: the sides form a closed,
// consistently oriented surface.
TEST(ElementType, SidesOfSolidsFormClosedOrientedSurface) {
    for (const ElementType type : linear_solids()) {
        const ElementTypeInfo& info = element_type_info(type);
        SCOPED_TRACE(info.name);

        std::map<std::pair<int, int>, int> directed;
        for (const Side& side : info.sides) {
            const auto corners = side.corners();
            for (std::size_t i = 0; i < corners.size(); ++i) {
                ++directed[{corners[i], corners[(i + 1) % corners.size()]}];
            }
        }
        std::set<std::pair<int, int>> from_sides;
        for (const auto& [edge, uses] : directed) {
            EXPECT_EQ(uses, 1);
            EXPECT_EQ(directed.count({edge.second, edge.first}), 1U);
            from_sides.emplace(std::min(edge.first, edge.second),
                               std::max(edge.first, edge.second));
        }
        std::set<std::pair<int, int>> from_edges;
        for (const Edge& edge : info.edges) {
            from_edges.emplace(std::min(edge[0], edge[1]), std::max(edge[0], edge[1]));
        }
        EXPECT_EQ(from_sides, from_edges);
    }
}

TEST(ElementType, SideNormalsOfSolidsPointOutward) {
    for (const ElementType type : linear_solids()) {
        const ElementTypeInfo& info = element_type_info(type);
        SCOPED_TRACE(info.name);
        const std::vector<Point> corners = reference_corners(type);
        ASSERT_EQ(static_cast<int>(corners.size()), info.corner_count);
        const Point element_center = centroid(corners);

        for (std::size_t s = 0; s < info.sides.size(); ++s) {
            std::vector<Point> polygon;
            for (const LocalNode node : info.sides[s].corners()) {
                polygon.push_back(corners[node]);
            }
            const Point normal = polygon_normal(polygon);
            const Point side_center = centroid(polygon);
            double outward = 0.0;
            for (std::size_t i = 0; i < 3; ++i) {
                outward += normal[i] * (side_center[i] - element_center[i]);
            }
            EXPECT_GT(outward, 0.0) << "side " << s;
        }
    }
}
