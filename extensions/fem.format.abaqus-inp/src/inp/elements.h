#pragma once

// Abaqus element types the Model IR can hold: their shape in the neutral numbering of the Model IR
// (docs/adr/0002-model-ir.md п. 7) and the kind of section they take. The node order of these
// Abaqus types is the neutral one, so connectivity is copied as it is.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fem::inp {

enum class Family : std::uint8_t { solid, shell, beam };

struct ElementKind {
    std::string_view abaqus;  ///< Upper case, e.g. "C3D8R".
    std::string_view shape;   ///< Neutral type name (`fem::model::ElementType`), e.g. "hex8".
    int nodes;
    Family family;
    bool reduced;  ///< Reduced integration.
    /// One of the types of stage 3 (docs/plan.md, "Объём Model IR"), which the shape, the kind
    /// of section and its integration describe completely; other types are read as their shape
    /// with a warning.
    bool basic;
};

[[nodiscard]] std::optional<ElementKind> element_kind(std::string_view abaqus_upper) noexcept;

/// Nodes of a neutral element type ("tet10" has 10), for the export, which reads meshes through
/// the C ABI without the core; nothing for an unknown name.
[[nodiscard]] std::optional<int> shape_nodes(std::string_view shape) noexcept;

/// The side of the neutral shape for Abaqus face `face` ("S1" is 1) of a solid; nothing for a
/// face the shape does not have or a shape without faces.
[[nodiscard]] std::optional<std::int32_t> neutral_side(std::string_view shape, int face) noexcept;

/// Corner nodes (0-based) of Abaqus face `face` of a solid shape; empty if there is none. For the
/// test that compares the faces with the sides of the Model IR.
[[nodiscard]] std::span<const std::uint8_t> abaqus_face_corners(std::string_view shape,
                                                                int face) noexcept;

}  // namespace fem::inp
