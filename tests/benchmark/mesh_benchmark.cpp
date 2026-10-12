// Benchmark of the Model IR mesh (task 1.2 of docs/plan.md): a structured hex mesh with one million
// nodes is built, stored in a document and traversed without copying its arrays.
//
// Allocations are counted per phase by replacing the global operator new. Once the arrays are
// filled, the later phases may allocate only small bookkeeping objects; the traversals allocate
// nothing. The program fails if that does not hold or if the arrays changed their address.
//
// Usage: fem_model_benchmark [nodes-per-axis]   (default 100, i.e. 1 000 000 nodes)

#include "fem/core/model/document.h"
#include "fem/core/model/mesh.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

std::atomic<std::size_t> allocated_bytes{0};
std::atomic<std::size_t> allocation_count{0};

}  // namespace

void* operator new(std::size_t size) {
    allocated_bytes += size;
    ++allocation_count;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

namespace {

using namespace fem::model;
using Clock = std::chrono::steady_clock;

// Bookkeeping allowed after the arrays are filled: a few small objects, never an array copy.
constexpr std::size_t small_allocation_limit = 64 * 1024;

struct Phase {
    const char* name;
    double milliseconds;
    std::size_t bytes;
    std::size_t allocations;
};

template <class F>
Phase measure(const char* name, F&& body) {
    const std::size_t bytes_before = allocated_bytes.load();
    const std::size_t count_before = allocation_count.load();
    const Clock::time_point start = Clock::now();
    body();
    const Clock::time_point stop = Clock::now();
    return {name, std::chrono::duration<double, std::milli>(stop - start).count(),
            allocated_bytes.load() - bytes_before, allocation_count.load() - count_before};
}

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
    }
    return condition;
}

bool nearly_equal(double actual, double expected) {
    return std::abs(actual - expected) <= 1e-9 * std::abs(expected);
}

}  // namespace

int main(int argc, char** argv) {
    const long per_axis = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 100;
    if (per_axis < 2 || per_axis > 1000) {
        std::fprintf(stderr, "nodes-per-axis must be between 2 and 1000\n");
        return 2;
    }
    const auto n = static_cast<std::size_t>(per_axis);
    const std::size_t node_count = n * n * n;
    const std::size_t cell_count = (n - 1) * (n - 1) * (n - 1);
    const auto node = [n](std::size_t i, std::size_t j, std::size_t k) {
        return static_cast<Index>(i + n * (j + n * k));
    };

    NodeArrays nodes;
    std::vector<Label> element_ids;
    std::vector<Index> connectivity;

    const Phase fill = measure("fill arrays", [&] {
        nodes.ids.reserve(node_count);
        nodes.x.reserve(node_count);
        nodes.y.reserve(node_count);
        nodes.z.reserve(node_count);
        for (std::size_t k = 0; k < n; ++k) {
            for (std::size_t j = 0; j < n; ++j) {
                for (std::size_t i = 0; i < n; ++i) {
                    nodes.ids.push_back(static_cast<Label>(nodes.ids.size() + 1));
                    nodes.x.push_back(static_cast<double>(i));
                    nodes.y.push_back(static_cast<double>(j));
                    nodes.z.push_back(static_cast<double>(k));
                }
            }
        }
        element_ids.reserve(cell_count);
        connectivity.reserve(cell_count * 8);
        for (std::size_t k = 0; k + 1 < n; ++k) {
            for (std::size_t j = 0; j + 1 < n; ++j) {
                for (std::size_t i = 0; i + 1 < n; ++i) {
                    element_ids.push_back(static_cast<Label>(element_ids.size() + 1));
                    // Neutral (VTK) hex8 order: bottom face counter-clockwise, then top face.
                    for (const std::size_t layer : {k, k + 1}) {
                        connectivity.push_back(node(i, j, layer));
                        connectivity.push_back(node(i + 1, j, layer));
                        connectivity.push_back(node(i + 1, j + 1, layer));
                        connectivity.push_back(node(i, j + 1, layer));
                    }
                }
            }
        }
    });

    const double* const x_data = nodes.x.data();
    const double* const y_data = nodes.y.data();
    const double* const z_data = nodes.z.data();
    const Label* const node_id_data = nodes.ids.data();
    const Index* const connectivity_data = connectivity.data();

    std::shared_ptr<const Mesh> mesh;
    const Phase build = measure("build Mesh (move + validate)", [&] {
        std::vector<ElementBlock> blocks;
        blocks.emplace_back(ElementType::hex8, std::move(element_ids), std::move(connectivity));
        mesh = std::make_shared<const Mesh>(std::move(nodes), std::move(blocks));
    });

    Document doc(UnitSystem::mm_t_s());
    ObjectId id;
    const Phase insert = measure("insert into Document",
                                 [&] { id = doc.create(mesh_object("box", std::move(mesh))); });

    double coordinate_sum = 0.0;
    const Phase node_pass = measure("traverse nodes", [&] {
        const std::shared_ptr<const Mesh> stored = doc.data<Mesh>(id);
        const std::span<const double> x = stored->x();
        const std::span<const double> y = stored->y();
        const std::span<const double> z = stored->z();
        for (std::size_t i = 0; i < x.size(); ++i) {
            coordinate_sum += x[i] + y[i] + z[i];
        }
    });

    double centroid_sum = 0.0;
    const Phase element_pass = measure("traverse elements (centroids)", [&] {
        const std::shared_ptr<const Mesh> stored = doc.data<Mesh>(id);
        const std::span<const double> x = stored->x();
        const std::span<const double> y = stored->y();
        const std::span<const double> z = stored->z();
        for (const ElementBlock& block : stored->blocks()) {
            const double weight = 1.0 / block.nodes_per_element();
            for (std::size_t e = 0; e < block.size(); ++e) {
                double sum = 0.0;
                for (const Index v : block.element_nodes(e)) {
                    sum += x[static_cast<std::size_t>(v)] + y[static_cast<std::size_t>(v)] +
                           z[static_cast<std::size_t>(v)];
                }
                centroid_sum += sum * weight;
            }
        }
    });

    const std::shared_ptr<const Mesh> stored = doc.data<Mesh>(id);
    const double array_megabytes =
        static_cast<double>(node_count * (3 * sizeof(double) + sizeof(Label)) +
                            cell_count * (sizeof(Label) + 8 * sizeof(Index))) /
        (1024.0 * 1024.0);
    std::printf("Model IR benchmark: %zu nodes, %zu hex8 elements, %.1f MiB of arrays\n\n",
                stored->node_count(), stored->element_count(), array_megabytes);
    std::printf("%-32s %12s %18s %12s\n", "phase", "time, ms", "allocated, bytes", "allocations");
    for (const Phase& phase : {fill, build, insert, node_pass, element_pass}) {
        std::printf("%-32s %12.1f %18zu %12zu\n", phase.name, phase.milliseconds, phase.bytes,
                    phase.allocations);
    }
    std::printf("\n");

    const double half_n = static_cast<double>(n) * static_cast<double>(n - 1) / 2.0;
    const double cells_per_axis = static_cast<double>(n - 1);
    const double expected_coordinate_sum = 3.0 * static_cast<double>(n * n) * half_n;
    const double expected_centroid_sum = 1.5 * std::pow(cells_per_axis, 4.0);

    bool ok = true;
    ok &= check(stored->node_count() == node_count, "node count");
    ok &= check(stored->element_count() == cell_count, "element count");
    ok &= check(stored->x().data() == x_data && stored->y().data() == y_data &&
                    stored->z().data() == z_data && stored->node_ids().data() == node_id_data,
                "node arrays were copied");
    ok &= check(stored->blocks()[0].connectivity().data() == connectivity_data,
                "connectivity was copied");
    ok &= check(build.bytes <= small_allocation_limit, "building the mesh allocated too much");
    ok &= check(insert.bytes <= small_allocation_limit, "inserting the mesh allocated too much");
    ok &= check(node_pass.bytes == 0, "traversing nodes allocated memory");
    ok &= check(element_pass.bytes == 0, "traversing elements allocated memory");
    ok &= check(nearly_equal(coordinate_sum, expected_coordinate_sum), "coordinate checksum");
    ok &= check(nearly_equal(centroid_sum, expected_centroid_sum), "centroid checksum");

    std::printf("%s\n", ok ? "OK: arrays were moved, never copied" : "FAILED");
    return ok ? 0 : 1;
}
