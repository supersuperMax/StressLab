// Blob store benchmark (task 2.2): saving and opening the bulk data of a mesh of a million nodes
// (a smaller one in Debug), what a local change adds to the store, and cleaning. Prints its
// timings and sizes (`ctest -L benchmark -V`); fails only if the data does not read back or a
// change of the mesh of a million nodes adds more than a tenth of it.

#include "fem/storage/blob_store.h"
#include "fem/storage/garbage.h"
#include "fem/storage/object_data_codec.h"

#include "../unit/storage/grid_mesh.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::storage::FolderBlobStore;
using fem::storage::testing::GridMesh;

#ifdef NDEBUG
constexpr int edge = 100;  // 1 030 301 nodes, 1 000 000 hex8.
#else
constexpr int edge = 40;
#endif

class Timer {
public:
    [[nodiscard]] double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

std::uintmax_t stored_size(const FolderBlobStore& store) {
    std::uintmax_t size = 0;
    for (const FolderBlobStore::File& file : store.files()) {
        size += file.size;
    }
    return size;
}

double megabytes(std::uintmax_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

}  // namespace

int main() {
    const fs::path folder = fs::temp_directory_path() / "stresslab-blob-benchmark";
    std::error_code ignored;
    fs::remove_all(folder, ignored);
    FolderBlobStore store(folder);

    GridMesh grid(edge, edge, edge);
    const auto mesh = grid.make();
    const std::size_t raw = mesh->node_count() * (8 + 3 * 8) + mesh->element_count() * (8 + 8 * 4);
    std::printf("mesh: %zu nodes, %zu hex8, %.1f MiB of arrays\n", mesh->node_count(),
                mesh->element_count(), megabytes(raw));

    Timer first;
    const std::string ref = fem::storage::put_object_data(*mesh, store);
    const double first_time = first.seconds();
    const std::uintmax_t first_size = stored_size(store);
    std::printf("first save: %.2f s, %zu files, %.1f MiB on disk (%.0f%% of the arrays)\n",
                first_time, store.files().size(), megabytes(first_size),
                100.0 * static_cast<double>(first_size) / static_cast<double>(raw));

    Timer again;
    const bool same = fem::storage::put_object_data(*mesh, store) == ref;
    std::printf("unchanged save: %.2f s\n", again.seconds());

    Timer load;
    const auto back = fem::storage::get_object_data(ref, store);
    std::printf("open: %.2f s\n", load.seconds());
    const bool equal = same && fem::storage::describe_object_data(*back) ==
                                   fem::storage::describe_object_data(*mesh);

    grid.move_nodes(grid.nodes.ids.size() / 3, grid.nodes.ids.size() / 100);
    Timer changed;
    const std::string moved = fem::storage::put_object_data(*grid.make(), store);
    const std::uintmax_t added = stored_size(store) - first_size;
    std::printf("1%% of the nodes moved: %.2f s, %.2f MiB added (%.1f%% of the first save)\n",
                changed.seconds(), megabytes(added),
                100.0 * static_cast<double>(added) / static_cast<double>(first_size));

    Timer clean;
    const std::vector<std::string> roots = {moved};
    const fem::storage::CleanStats stats = fem::storage::collect_garbage(store, roots);
    std::printf("clean: %.2f s, %zu files removed (%.2f MiB), %zu kept\n", clean.seconds(),
                stats.removed, megabytes(stats.freed_bytes), stats.kept);

    fs::remove_all(folder, ignored);
    if (!equal) {
        std::printf("FAILED: the mesh does not read back\n");
        return 1;
    }
    // A change costs about two pieces per array it touches: little only next to a large mesh.
    if (edge >= 100 && added * 10 > first_size) {
        std::printf("FAILED: a local change added more than a tenth of the mesh\n");
        return 1;
    }
    return 0;
}
