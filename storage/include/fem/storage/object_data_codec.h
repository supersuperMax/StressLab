#pragma once

#include "fem/core/model/object_data.h"
#include "fem/storage/blob_store.h"
#include "fem/storage/chunking.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Bulk data of objects (meshes, sets and the named arrays of modules) as blocks of the blob store,
// version 2 of the encoding (docs/adr/0018-blob-store.md). The data of an object is a table of
// contents: the magic "SLDATA\0\0", the version and kind as uint32, then its arrays in a fixed
// order (for a mesh: node ids, x, y, z, the element blocks with their type, ids and connectivity;
// for named arrays, kind 6 of task 3.1: each array by name with its name, item type and width as
// uint32, docs/adr/0023-structural-model-and-results-ir.md). An array is its item
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

/// Stores `data` (a Mesh, NodeSet, ElementSet, Surface or ArrayData) in `blobs`: the pieces of its
/// large arrays, then its table of contents, whose reference it returns. Pieces already in the
/// store are not written again. Throws ProjectError(invalid_data) for other data and what `blobs`
/// throws.
[[nodiscard]] std::string put_object_data(const model::ObjectData& data, BlobStore& blobs,
                                          const ChunkLimits& limits = {});

/// The data whose table of contents is the block `ref`. Throws ProjectError: missing_blob if the
/// table or one of its pieces is not in `blobs`, corrupt_blob if a block is not such data, is
/// truncated or has bytes left over, or the arrays make an invalid mesh or set.
[[nodiscard]] std::shared_ptr<const model::ObjectData> get_object_data(std::string_view ref,
                                                                       const BlobStore& blobs);

/// The pieces a table of contents refers to, in its order, repeats included; a table of bytes
/// (`put_byte_data`) too. Throws ProjectError(corrupt_blob) if `table` is not a table of contents.
[[nodiscard]] std::vector<std::string> object_data_pieces(std::span<const std::byte> table);

/// Gives the next bytes of a stream: fills the start of the buffer and returns how many bytes it
/// put there, 0 at the end. It may throw to stop the reading (a cancelled operation).
using ByteSource = std::function<std::size_t(std::span<std::byte> buffer)>;
/// Takes the next bytes of a stream.
using ByteSink = std::function<void(std::span<const std::byte> bytes)>;

/// Stores a stream of bytes of any length (a file of a pinned result, task 2.5) as data of the
/// kind "bytes": a table of contents with one array of bytes, cut into pieces by `chunk_ends` as
/// it is read, so the stream is never in memory whole and a file that changes in one place shares
/// its other pieces with the old one. Returns the reference of the table, a root for the garbage
/// collection like the data of an object. Throws what `read` and `blobs` throw.
[[nodiscard]] std::string put_byte_data(const ByteSource& read, BlobStore& blobs,
                                        const ChunkLimits& limits = {});

/// Gives the bytes whose table is the block `ref` to `write`, piece by piece, and returns how
/// many there were. Throws ProjectError: missing_blob if the table or a piece is not in `blobs`,
/// corrupt_blob if the table is not of the kind "bytes" or a piece has another size than the
/// table says; and what `write` throws.
std::uint64_t get_byte_data(std::string_view ref, const BlobStore& blobs, const ByteSink& write);

/// What a project file says about the data of an object, so that a diff of versions can show
/// it without the block: {"kind": "mesh", "nodes": 1234, "elements": 5678}, or
/// {"kind": "node_set" | "element_set" | "surface", "size": 12}, or for named arrays
/// {"kind": "arrays", "size": <rows>, "arrays": {"values": {"type": "float64", "width": 3}}}.
[[nodiscard]] nlohmann::json describe_object_data(const model::ObjectData& data);

}  // namespace fem::storage
