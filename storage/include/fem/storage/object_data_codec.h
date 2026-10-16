#pragma once

#include "fem/core/model/object_data.h"
#include "fem/storage/blob_store.h"
#include "fem/storage/chunking.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Bulk data of objects (meshes and sets) as blocks of the blob store, version 2 of the encoding
// (docs/adr/0018-blob-store.md). The data of an object is a table of contents: the magic
// "SLDATA\0\0", the version and kind as uint32, then its arrays in a fixed order (for a mesh: node
// ids, x, y, z, the element blocks with their type, ids and connectivity). An array is its item
// count and either its items, if they take at most `inline_array_bytes`, or the list of its
// pieces: each a SHA-256 digest of 32 bytes and an item count. A piece is a block of its own that
// holds the items of a slice of the array, regrouped by `shuffle_bytes`. Arrays are cut by
// `chunk_ends`, so a local change of a mesh changes a few pieces and the small table, and the
// rest of the mesh is not stored again. Every number is little-endian whatever the computer is, so
// a block has the same bytes (and hash) on every OS.
namespace fem::storage {

inline constexpr std::uint32_t object_data_codec_version = 2;

/// Arrays up to this size stay inside the table of contents; larger ones are cut into pieces.
inline constexpr std::size_t inline_array_bytes = 16 * 1024;

/// Stores `data` (a Mesh, NodeSet, ElementSet or Surface) in `blobs`: the pieces of its large
/// arrays, then its table of contents, whose reference it returns. Pieces already in the store are
/// not written again. Throws ProjectError(invalid_data) for other data and what `blobs` throws.
[[nodiscard]] std::string put_object_data(const model::ObjectData& data, BlobStore& blobs,
                                          const ChunkLimits& limits = {});

/// The data whose table of contents is the block `ref`. Throws ProjectError: missing_blob if the
/// table or one of its pieces is not in `blobs`, corrupt_blob if a block is not such data, is
/// truncated or has bytes left over, or the arrays make an invalid mesh or set.
[[nodiscard]] std::shared_ptr<const model::ObjectData> get_object_data(std::string_view ref,
                                                                       const BlobStore& blobs);

/// The pieces a table of contents refers to, in its order, repeats included. Throws
/// ProjectError(corrupt_blob) if `table` is not a table of contents.
[[nodiscard]] std::vector<std::string> object_data_pieces(std::span<const std::byte> table);

/// What a project file says about the data of an object, so that a diff of versions can show
/// it without the block: {"kind": "mesh", "nodes": 1234, "elements": 5678}, or
/// {"kind": "node_set" | "element_set" | "surface", "size": 12}.
[[nodiscard]] nlohmann::json describe_object_data(const model::ObjectData& data);

}  // namespace fem::storage
