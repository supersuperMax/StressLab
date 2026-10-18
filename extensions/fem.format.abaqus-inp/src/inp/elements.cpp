#include "inp/elements.h"

#include <vector>

namespace fem::inp {

namespace {

constexpr std::array kinds{
    // Solids.
    ElementKind{"C3D4", "tet4", 4, Family::solid, false, true},
    ElementKind{"C3D4H", "tet4", 4, Family::solid, false, false},
    ElementKind{"C3D10", "tet10", 10, Family::solid, false, true},
    ElementKind{"C3D10H", "tet10", 10, Family::solid, false, false},
    ElementKind{"C3D10M", "tet10", 10, Family::solid, false, false},
    ElementKind{"C3D10MH", "tet10", 10, Family::solid, false, false},
    ElementKind{"C3D10HS", "tet10", 10, Family::solid, false, false},
    ElementKind{"C3D10I", "tet10", 10, Family::solid, false, false},
    ElementKind{"C3D8", "hex8", 8, Family::solid, false, true},
    ElementKind{"C3D8H", "hex8", 8, Family::solid, false, false},
    ElementKind{"C3D8I", "hex8", 8, Family::solid, false, false},
    ElementKind{"C3D8IH", "hex8", 8, Family::solid, false, false},
    ElementKind{"C3D8R", "hex8", 8, Family::solid, true, true},
    ElementKind{"C3D8RH", "hex8", 8, Family::solid, true, false},
    ElementKind{"C3D20", "hex20", 20, Family::solid, false, true},
    ElementKind{"C3D20H", "hex20", 20, Family::solid, false, false},
    ElementKind{"C3D20R", "hex20", 20, Family::solid, true, true},
    ElementKind{"C3D20RH", "hex20", 20, Family::solid, true, false},
    ElementKind{"C3D6", "wedge6", 6, Family::solid, false, false},
    ElementKind{"C3D6H", "wedge6", 6, Family::solid, false, false},
    ElementKind{"C3D15", "wedge15", 15, Family::solid, false, false},
    ElementKind{"C3D15H", "wedge15", 15, Family::solid, false, false},
    // Shells.
    ElementKind{"S3", "tri3", 3, Family::shell, false, true},
    ElementKind{"S3R", "tri3", 3, Family::shell, true, false},
    ElementKind{"S4", "quad4", 4, Family::shell, false, true},
    ElementKind{"S4R", "quad4", 4, Family::shell, true, true},
    ElementKind{"S4R5", "quad4", 4, Family::shell, true, false},
    ElementKind{"S6", "tri6", 6, Family::shell, false, false},
    ElementKind{"S8R", "quad8", 8, Family::shell, true, false},
    ElementKind{"S8R5", "quad8", 8, Family::shell, true, false},
    // Beams.
    ElementKind{"B31", "line2", 2, Family::beam, false, true},
    ElementKind{"B31H", "line2", 2, Family::beam, false, false},
    ElementKind{"B32", "line3", 3, Family::beam, false, false},
    ElementKind{"B32H", "line3", 3, Family::beam, false, false},
};

struct Face {
    std::uint8_t size;
    std::array<std::uint8_t, 4> corners;
    std::int32_t side;  // Side of the neutral shape with the same corners.
};

// Faces of the Abaqus manual (section "Three-dimensional solid element library"), 0-based, and
// the neutral sides of core/src/model/element_type.cpp with the same corners; the test
// `InpElements.FacesMatchTheSidesOfTheModel` checks the pairs against the core.
constexpr std::array<Face, 4> tet_faces{{
    {3, {0, 1, 2}, 3},
    {3, {0, 3, 1}, 0},
    {3, {1, 3, 2}, 1},
    {3, {2, 3, 0}, 2},
}};
constexpr std::array<Face, 6> hex_faces{{
    {4, {0, 1, 2, 3}, 4},
    {4, {4, 7, 6, 5}, 5},
    {4, {0, 4, 5, 1}, 2},
    {4, {1, 5, 6, 2}, 1},
    {4, {2, 6, 7, 3}, 3},
    {4, {3, 7, 4, 0}, 0},
}};
constexpr std::array<Face, 5> wedge_faces{{
    {3, {0, 1, 2}, 0},
    {3, {3, 5, 4}, 1},
    {4, {0, 3, 4, 1}, 2},
    {4, {1, 4, 5, 2}, 3},
    {4, {2, 5, 3, 0}, 4},
}};

std::span<const Face> faces_of(std::string_view shape) noexcept {
    if (shape == "tet4" || shape == "tet10") {
        return tet_faces;
    }
    if (shape == "hex8" || shape == "hex20") {
        return hex_faces;
    }
    if (shape == "wedge6" || shape == "wedge15") {
        return wedge_faces;
    }
    return {};
}

}  // namespace

std::optional<ElementKind> element_kind(std::string_view abaqus_upper) noexcept {
    for (const ElementKind& kind : kinds) {
        if (kind.abaqus == abaqus_upper) {
            return kind;
        }
    }
    return std::nullopt;
}

std::optional<std::int32_t> neutral_side(std::string_view shape, int face) noexcept {
    const std::span<const Face> faces = faces_of(shape);
    if (face < 1 || static_cast<std::size_t>(face) > faces.size()) {
        return std::nullopt;
    }
    return faces[static_cast<std::size_t>(face) - 1].side;
}

std::span<const std::uint8_t> abaqus_face_corners(std::string_view shape, int face) noexcept {
    const std::span<const Face> faces = faces_of(shape);
    if (face < 1 || static_cast<std::size_t>(face) > faces.size()) {
        return {};
    }
    const Face& f = faces[static_cast<std::size_t>(face) - 1];
    return {f.corners.data(), f.size};
}

}  // namespace fem::inp
