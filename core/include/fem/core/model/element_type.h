#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fem::model {

/// Element shapes of the neutral mesh representation.
///
/// Neutral numbering (docs/adr/0002-model-ir.md): node order is the VTK cell order, which is also
/// the internal order of meshio. Format modules convert to and from it on import and export.
/// Quadratic types list the corner nodes first, then one mid-edge node per edge in the order of
/// `ElementTypeInfo::edges`. The type describes topology only; element formulation (solid, shell,
/// beam, reduced integration...) is not part of it.
enum class ElementType : std::uint8_t {
    vertex,
    line2,
    line3,
    tri3,
    tri6,
    quad4,
    quad8,
    tet4,
    tet10,
    pyramid5,
    pyramid13,
    wedge6,
    wedge15,
    hex8,
    hex20,
};

inline constexpr std::array all_element_types = {
    ElementType::vertex,  ElementType::line2,    ElementType::line3,     ElementType::tri3,
    ElementType::tri6,    ElementType::quad4,    ElementType::quad8,     ElementType::tet4,
    ElementType::tet10,   ElementType::pyramid5, ElementType::pyramid13, ElementType::wedge6,
    ElementType::wedge15, ElementType::hex8,     ElementType::hex20,
};

/// Index of a node inside one element, 0-based.
using LocalNode = std::uint8_t;

/// Edge of the reference element as a pair of corner nodes.
using Edge = std::array<LocalNode, 2>;

/// Side of the reference element, given by its corner nodes.
///
/// Sides are numbered per shape: for 3D shapes they are the faces, ordered as in VTK, with corners
/// ordered so that the right-hand rule gives the outward normal; for 2D shapes they are the edges;
/// for 1D shapes they are the two end points. Surfaces (`Surface`) refer to sides by this number.
struct Side {
    std::uint8_t size = 0;
    std::array<LocalNode, 4> nodes{};

    [[nodiscard]] constexpr std::span<const LocalNode> corners() const noexcept {
        return {nodes.data(), size};
    }
};

struct ElementTypeInfo {
    ElementType type;
    std::string_view name;  ///< Stable identifier written to project files, e.g. "tet10".
    int dimension;          ///< 0 for vertex, 1 for lines, 2 for surfaces, 3 for volumes.
    int node_count;         ///< Nodes per element.
    int corner_count;       ///< Corner nodes; equals node_count for linear types.
    ElementType linear;     ///< Same shape with corner nodes only.
    std::span<const Edge> edges;
    std::span<const Side> sides;
};

[[nodiscard]] const ElementTypeInfo& element_type_info(ElementType type) noexcept;

[[nodiscard]] inline int nodes_per_element(ElementType type) noexcept {
    return element_type_info(type).node_count;
}

[[nodiscard]] inline std::string_view to_string(ElementType type) noexcept {
    return element_type_info(type).name;
}

[[nodiscard]] std::optional<ElementType> parse_element_type(std::string_view name) noexcept;

}  // namespace fem::model
