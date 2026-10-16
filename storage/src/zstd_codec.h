#pragma once

#include "fem/storage/blob_store.h"

#include <cstddef>
#include <span>

// zstd frames of the blocks on disk; zstd itself stays out of the public headers.
namespace fem::storage::detail {

/// The largest block a frame may unpack to. Pieces of arrays are at most a few hundred KiB and
/// tables of contents a few MiB even for meshes of billions of elements, so a frame that claims
/// more is damaged, and it is refused before anything is allocated.
inline constexpr std::size_t max_block_size = std::size_t{256} * 1024 * 1024;

/// One zstd frame with the size of `bytes` in its header. Throws ProjectError(io) if zstd fails.
[[nodiscard]] Bytes compress(std::span<const std::byte> bytes);

/// The bytes of a frame made by `compress`. Throws ProjectError(corrupt_blob) unless `frame` is
/// exactly one whole frame that states its size, at most `max_block_size`, and unpacks to it.
[[nodiscard]] Bytes decompress(std::span<const std::byte> frame);

}  // namespace fem::storage::detail
