#pragma once

#include "fem/storage/blob_store.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// Garbage collection of the blob store: the "Clean project" command (docs/architecture.md §3.1,
// docs/adr/0018-blob-store.md). Blocks are never changed, so the old data of a mesh stays in the
// store after every save; the collection removes the blocks nothing refers to any more.
namespace fem::storage {

/// What a collection did.
struct CleanStats {
    std::size_t removed = 0;         ///< Files removed: blocks and leftovers.
    std::uintmax_t freed_bytes = 0;  ///< Their size on disk.
    std::size_t kept = 0;            ///< Blocks kept.
    std::uintmax_t kept_bytes = 0;   ///< Their size on disk.
};

/// Removes from `blobs` every block not reachable from `roots` (tables of contents of object data,
/// `object_data_codec.h`, and the pieces they list), files that are not blocks (temporary files a
/// crash left), and the folders that become empty. Everything reachable is found before anything
/// is removed: if a root is missing or damaged it throws ProjectError (missing_blob,
/// corrupt_blob) and removes nothing, since the blocks of a root it cannot read would be lost. A
/// piece a table lists but the store lacks does not stop it: nothing more is lost by cleaning.
/// Throws ProjectError(io) if the folder cannot be listed or a file cannot be removed.
CleanStats collect_garbage(FolderBlobStore& blobs, std::span<const std::string> roots);

}  // namespace fem::storage
