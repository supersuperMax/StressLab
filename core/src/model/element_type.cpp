#include "fem/core/model/element_type.h"

#include <array>
#include <cstddef>

namespace fem::model {

namespace {

// Edges in the order of the mid-edge nodes of the quadratic types (VTK). Sides of 3D shapes are
// the faces of VTK's cells (vtkTetra::GetFaceArray and the like), outward by the right-hand rule.

constexpr std::array<Edge, 1> line_edges{{{0, 1}}};
constexpr std::array<Side, 2> line_sides{{{1, {0}}, {1, {1}}}};

constexpr std::array<Edge, 3> tri_edges{{{0, 1}, {1, 2}, {2, 0}}};
constexpr std::array<Side, 3> tri_sides{{{2, {0, 1}}, {2, {1, 2}}, {2, {2, 0}}}};

constexpr std::array<Edge, 4> quad_edges{{{0, 1}, {1, 2}, {2, 3}, {3, 0}}};
constexpr std::array<Side, 4> quad_sides{{{2, {0, 1}}, {2, {1, 2}}, {2, {2, 3}}, {2, {3, 0}}}};

constexpr std::array<Edge, 6> tet_edges{{{0, 1}, {1, 2}, {2, 0}, {0, 3}, {1, 3}, {2, 3}}};
constexpr std::array<Side, 4> tet_sides{{
    {3, {0, 1, 3}},
    {3, {1, 2, 3}},
    {3, {2, 0, 3}},
    {3, {0, 2, 1}},
}};

constexpr std::array<Edge, 8> pyramid_edges{{
    {0, 1},
    {1, 2},
    {2, 3},
    {3, 0},
    {0, 4},
    {1, 4},
    {2, 4},
    {3, 4},
}};
constexpr std::array<Side, 5> pyramid_sides{{
    {4, {0, 3, 2, 1}},
    {3, {0, 1, 4}},
    {3, {1, 2, 4}},
    {3, {2, 3, 4}},
    {3, {3, 0, 4}},
}};

constexpr std::array<Edge, 9> wedge_edges{{
    {0, 1},
    {1, 2},
    {2, 0},
    {3, 4},
    {4, 5},
    {5, 3},
    {0, 3},
    {1, 4},
    {2, 5},
}};
constexpr std::array<Side, 5> wedge_sides{{
    {3, {0, 1, 2}},
    {3, {3, 5, 4}},
    {4, {0, 3, 4, 1}},
    {4, {1, 4, 5, 2}},
    {4, {2, 5, 3, 0}},
}};

constexpr std::array<Edge, 12> hex_edges{{
    {0, 1},
    {1, 2},
    {2, 3},
    {3, 0},
    {4, 5},
    {5, 6},
    {6, 7},
    {7, 4},
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7},
}};
constexpr std::array<Side, 6> hex_sides{{
    {4, {0, 4, 7, 3}},
    {4, {1, 2, 6, 5}},
    {4, {0, 1, 5, 4}},
    {4, {3, 7, 6, 2}},
    {4, {0, 3, 2, 1}},
    {4, {4, 5, 6, 7}},
}};

using T = ElementType;

// Indexed by the enum value; the order is checked by a unit test.
constexpr std::array<ElementTypeInfo, all_element_types.size()> infos{{
    {T::vertex, "vertex", 0, 1, 1, T::vertex, {}, {}},
    {T::line2, "line2", 1, 2, 2, T::line2, line_edges, line_sides},
    {T::line3, "line3", 1, 3, 2, T::line2, line_edges, line_sides},
    {T::tri3, "tri3", 2, 3, 3, T::tri3, tri_edges, tri_sides},
    {T::tri6, "tri6", 2, 6, 3, T::tri3, tri_edges, tri_sides},
    {T::quad4, "quad4", 2, 4, 4, T::quad4, quad_edges, quad_sides},
    {T::quad8, "quad8", 2, 8, 4, T::quad4, quad_edges, quad_sides},
    {T::tet4, "tet4", 3, 4, 4, T::tet4, tet_edges, tet_sides},
    {T::tet10, "tet10", 3, 10, 4, T::tet4, tet_edges, tet_sides},
    {T::pyramid5, "pyramid5", 3, 5, 5, T::pyramid5, pyramid_edges, pyramid_sides},
    {T::pyramid13, "pyramid13", 3, 13, 5, T::pyramid5, pyramid_edges, pyramid_sides},
    {T::wedge6, "wedge6", 3, 6, 6, T::wedge6, wedge_edges, wedge_sides},
    {T::wedge15, "wedge15", 3, 15, 6, T::wedge6, wedge_edges, wedge_sides},
    {T::hex8, "hex8", 3, 8, 8, T::hex8, hex_edges, hex_sides},
    {T::hex20, "hex20", 3, 20, 8, T::hex8, hex_edges, hex_sides},
}};

}  // namespace

const ElementTypeInfo& element_type_info(ElementType type) noexcept {
    return infos[static_cast<std::size_t>(type)];
}

std::optional<ElementType> parse_element_type(std::string_view name) noexcept {
    for (const ElementTypeInfo& info : infos) {
        if (info.name == name) {
            return info.type;
        }
    }
    return std::nullopt;
}

}  // namespace fem::model
