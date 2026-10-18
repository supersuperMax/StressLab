#pragma once

// The model as the export reads it (task 3.3): the objects of the Model IR in tree order, with
// views of their bulk data. The module fills it from the C ABI (module.cpp); the tests make it
// from an import with `model_of`, so that the writer is tested without the platform.

#include "inp/import.h"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace fem::inp {

struct MeshBlockView {
    std::string shape;  ///< Neutral element type, e.g. "tet10".
    std::span<const std::int64_t> ids;
    std::span<const std::int32_t> connectivity;
};

struct MeshView {
    std::span<const std::int64_t> node_ids;
    std::span<const double> x, y, z;
    std::vector<MeshBlockView> blocks;
};

struct SetView {
    SetKind kind = SetKind::node;
    /// Node or global element indices; for a surface, pairs (element, side).
    std::span<const std::int32_t> indices;
};

struct ModelObject {
    std::int64_t id = 0;
    std::string type;
    std::string owner;
    std::string name;
    std::int64_t parent = 0;  ///< 0 for a root object.
    std::vector<std::int64_t> dependencies;
    nlohmann::json properties = nlohmann::json::object();
    bool opaque = false;  ///< An object of a disabled module.
    std::variant<std::monostate, MeshView, SetView> data;
};

struct Model {
    /// Roots in order, each followed by its subtree; children in creation order.
    std::vector<ModelObject> objects;
    /// The unit system as symbols: {"length": "mm", "mass": "t", "time": "s", ...}.
    nlohmann::json units = nlohmann::json::object();
};

/// The model an import would make: ids are positions in `result.items` plus one, dependencies
/// sorted as the Model IR keeps them. The views point into `result`, which must outlive the model.
[[nodiscard]] Model model_of(const Import& result);

}  // namespace fem::inp
