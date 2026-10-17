#include "fem/storage/garbage.h"

#include "fem/core/io/files.h"
#include "fem/storage/error.h"
#include "fem/storage/object_data_codec.h"

#include "folders.h"

#include <set>

namespace fem::storage {

std::set<std::string, std::less<>> reachable_blobs(const BlobStore& blobs,
                                                   std::span<const std::string> roots) {
    std::set<std::string, std::less<>> reachable;
    std::set<std::string, std::less<>> tables;
    for (const std::string& root : roots) {
        if (!tables.insert(root).second) {
            continue;
        }
        reachable.insert(root);
        try {
            for (std::string& piece : object_data_pieces(blobs.get(root))) {
                reachable.insert(std::move(piece));
            }
        } catch (const ProjectError& error) {
            throw ProjectError(error.code(),
                               "the block " + root + " cannot be read (" + error.what() + ")");
        }
    }
    return reachable;
}

CleanStats collect_garbage(FolderBlobStore& blobs, std::span<const std::string> roots) {
    std::set<std::string, std::less<>> reachable;
    try {
        reachable = reachable_blobs(blobs, roots);
    } catch (const ProjectError& error) {
        throw ProjectError(error.code(), std::string("nothing was cleaned: ") + error.what());
    }

    CleanStats stats;
    for (const FolderBlobStore::File& file : blobs.files()) {
        if (!file.ref.empty() && reachable.contains(file.ref)) {
            ++stats.kept;
            stats.kept_bytes += file.size;
            continue;
        }
        if (!blobs.remove(file.path)) {
            throw ProjectError(ProjectErrc::io, "cannot remove the file", io::display(file.path));
        }
        ++stats.removed;
        stats.freed_bytes += file.size;
    }
    detail::remove_empty_folders(blobs.folder());
    return stats;
}

}  // namespace fem::storage
