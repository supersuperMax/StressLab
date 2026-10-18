#pragma once

// An input file as objects of the Model IR, before they are created: the module creates them in
// this order through the C ABI (module.cpp), the tests look at them directly. What the objects
// mean: docs/extensions/fem.format.abaqus-inp.md.

#include "inp/encoding.h"
#include "inp/problem.h"
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace fem::inp {

inline constexpr std::string_view module_owner = "fem.format.abaqus-inp";
inline constexpr std::string_view core_owner = "fem.core";
inline constexpr std::string_view structural_owner = "fem.model.structural";

/// Position of an item in `Import::items`; `no_item` for none.
using ItemRef = std::size_t;
inline constexpr ItemRef no_item = std::numeric_limits<std::size_t>::max();

struct MeshBlock {
    std::string shape;   ///< Neutral element type, e.g. "tet10".
    std::string abaqus;  ///< Abaqus element type, e.g. "C3D10M".
    std::vector<std::int64_t> ids;
    std::vector<std::int32_t> connectivity;
};

struct MeshData {
    std::vector<std::int64_t> node_ids;
    std::vector<double> x, y, z;
    std::vector<MeshBlock> blocks;
};

enum class SetKind : std::uint8_t { node, element, surface };

struct SetData {
    SetKind kind = SetKind::node;
    /// Node or global element indices, sorted and unique; for a surface, pairs (element, side).
    std::vector<std::int32_t> indices;
};

struct Item {
    std::string type;
    std::string owner;
    std::string name;
    nlohmann::json properties = nlohmann::json::object();
    ItemRef parent = no_item;
    std::vector<ItemRef> dependencies;
    std::variant<std::monostate, MeshData, SetData> data;
};

struct Import {
    /// In creation order: a parent and every dependency come before the item.
    std::vector<Item> items;
    std::vector<Problem> problems;
    ItemRef input = no_item;  ///< The `abaqus_input` object.
};

/// Reads a file. Throws InpError if it cannot be imported at all; everything else becomes
/// objects, passthrough blocks and problems.
[[nodiscard]] Import import_file(const std::filesystem::path& file, Encoding encoding);

}  // namespace fem::inp
