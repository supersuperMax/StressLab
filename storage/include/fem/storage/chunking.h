#pragma once

#include "fem/storage/blob_store.h"

#include <cstddef>
#include <span>
#include <vector>

// How large arrays are cut into blocks (docs/adr/0018-blob-store.md): content-defined chunking
// with a gear hash, in the manner of FastCDC. A cut depends only on the bytes just before it, so
// a change in one place of an array moves the cuts around it and nowhere else: the other pieces
// keep their bytes and hashes, and a local change of a mesh adds only a few blocks to the store.
namespace fem::storage {

/// Sizes of pieces in bytes. Only the store size depends on them: a reader takes pieces of any
/// size, so they can change without changing the format.
struct ChunkLimits {
    std::size_t min = 16 * 1024;
    std::size_t average = 64 * 1024;
    std::size_t max = 256 * 1024;
};

/// Where `bytes` are cut: the end of each piece, the last one `bytes.size()`; none for no bytes.
/// Every end is a multiple of `item_size` (an array is never cut inside an item), and every piece
/// but the last is between `limits.min` and `limits.max` bytes, rounded to whole items. The
/// result depends on the bytes only, the same on every computer.
[[nodiscard]] std::vector<std::size_t> chunk_ends(std::span<const std::byte> bytes,
                                                  std::size_t item_size,
                                                  const ChunkLimits& limits = {});

/// The bytes of items of `item_size` bytes regrouped by their position in the item: the first
/// byte of every item, then the second... (the "shuffle" filter of HDF5 and Blosc). Neighbouring
/// numbers share their high bytes, so zstd packs the result much better than the numbers.
/// `bytes.size()` must be a multiple of `item_size`.
[[nodiscard]] Bytes shuffle_bytes(std::span<const std::byte> bytes, std::size_t item_size);
/// The inverse of `shuffle_bytes`.
[[nodiscard]] Bytes unshuffle_bytes(std::span<const std::byte> bytes, std::size_t item_size);

}  // namespace fem::storage
