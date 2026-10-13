#pragma once

// Model data in the protocol of out-of-process modules (docs/extensions/protocol.md, "Data"):
// meshes and sets as JSON whose arrays are in shared memory or written inline.

#include "fem/core/ipc/shared_memory.h"
#include "fem/core/model/document.h"
#include "fem/core/model/object.h"
#include "fem/core/model/object_data.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <optional>

namespace fem::extension::detail {

/// Bulk data a module sent: `{"kind": "mesh" | "nodeSet" | "elementSet" | "surface", ...}`.
/// Arrays are copied out of the module's shared memory. Throws ExtensionError(command_failed)
/// saying what is wrong, and ModelError for a mesh the model refuses.
[[nodiscard]] std::shared_ptr<const model::ObjectData> data_from_json(const nlohmann::json& json);

/// Bulk data for a module. With `threshold`, the arrays (if they hold at least that many bytes
/// together) go into one new segment of shared memory, returned in `segment`; otherwise they are
/// written inline. Null for data the protocol does not describe.
[[nodiscard]] nlohmann::json data_to_json(const model::ObjectData& data,
                                          std::optional<std::size_t> threshold,
                                          std::optional<ipc::SharedMemory>& segment);

/// An object without its bulk data: id, type, owner, name, schemaVersion, properties, parent,
/// children, dependencies, opaque and the kind of its data (null if none).
[[nodiscard]] nlohmann::json object_to_json(const model::Document& document,
                                            const model::Object& object);

}  // namespace fem::extension::detail
