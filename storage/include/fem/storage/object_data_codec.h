#pragma once

#include "fem/core/model/object_data.h"
#include "fem/storage/blob_store.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <span>

// Bulk data of objects (meshes and sets) as blocks of the blob store. Version 1 of the encoding,
// until task 2.2 moves it to HDF5 with compression: the magic "SLDATA\0\0", the version and kind
// as uint32, then the arrays, each as a uint64 count and its items. Every number is little-endian
// whatever the computer is, so a block has the same bytes (and hash) on every OS.
namespace fem::storage {

inline constexpr std::uint32_t object_data_codec_version = 1;

/// The block of `data`: a Mesh, NodeSet, ElementSet or Surface. Throws ProjectError(invalid_data)
/// for other data.
[[nodiscard]] Bytes encode_object_data(const model::ObjectData& data);

/// The data of a block. Throws ProjectError(corrupt_blob) if the block is not such data, is
/// truncated, has bytes left over, or holds an invalid mesh or set.
[[nodiscard]] std::shared_ptr<const model::ObjectData> decode_object_data(
    std::span<const std::byte> bytes);

/// What a project file says about the data of an object, so that a diff of versions can show
/// it without the block: {"kind": "mesh", "nodes": 1234, "elements": 5678}, or
/// {"kind": "node_set" | "element_set" | "surface", "size": 12}.
[[nodiscard]] nlohmann::json describe_object_data(const model::ObjectData& data);

}  // namespace fem::storage
