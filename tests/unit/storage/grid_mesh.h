#pragma once

#include "fem/core/model/element_type.h"
#include "fem/core/model/mesh.h"

#include <cstddef>
#include <memory>
#include <random>
#include <utility>
#include <vector>

// A regular mesh of any size for the tests and the benchmark of the blob store (task 2.2).
namespace fem::storage::testing {

/// The arrays of a box of nx * ny * nz hex8 elements, numbered along x, then y, then z, as meshers
/// number them: the nodes of a region are close in the arrays. Coordinates are jittered in all
/// their digits, as those a mesher computes, so they do not compress better than a real mesh.
struct GridMesh {
    model::NodeArrays nodes;
    std::vector<model::Label> element_ids;
    std::vector<model::Index> connectivity;

    GridMesh(int nx, int ny, int nz) {
        const auto node = [nx, ny](int i, int j, int k) {
            return static_cast<model::Index>(i + (nx + 1) * (j + (ny + 1) * k));
        };
        std::mt19937_64 engine(2026);
        std::uniform_real_distribution<double> jitter(-1e-4, 1e-4);
        for (int k = 0; k <= nz; ++k) {
            for (int j = 0; j <= ny; ++j) {
                for (int i = 0; i <= nx; ++i) {
                    nodes.ids.push_back(static_cast<model::Label>(nodes.ids.size()) + 1);
                    nodes.x.push_back(0.01 * i + jitter(engine));
                    nodes.y.push_back(0.01 * j + jitter(engine));
                    nodes.z.push_back(0.01 * k + jitter(engine));
                }
            }
        }
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    element_ids.push_back(static_cast<model::Label>(element_ids.size()) + 1);
                    for (const model::Index n :
                         {node(i, j, k), node(i + 1, j, k), node(i + 1, j + 1, k),
                          node(i, j + 1, k), node(i, j, k + 1), node(i + 1, j, k + 1),
                          node(i + 1, j + 1, k + 1), node(i, j + 1, k + 1)}) {
                        connectivity.push_back(n);
                    }
                }
            }
        }
    }

    [[nodiscard]] std::shared_ptr<const model::Mesh> make() const {
        std::vector<model::ElementBlock> blocks;
        blocks.emplace_back(model::ElementType::hex8, element_ids, connectivity);
        return std::make_shared<const model::Mesh>(nodes, std::move(blocks));
    }

    /// Moves the nodes [first, first + count) a little: a local change of the geometry.
    void move_nodes(std::size_t first, std::size_t count) {
        for (std::size_t n = first; n < first + count; ++n) {
            nodes.x[n] += 1e-3;
            nodes.y[n] -= 2e-3;
            nodes.z[n] += 3e-3;
        }
    }

    /// Removes the elements [first, first + count): what follows shifts in the arrays.
    void remove_elements(std::size_t first, std::size_t count) {
        const auto at = [](auto& values, std::size_t index) {
            return values.begin() + static_cast<std::ptrdiff_t>(index);
        };
        element_ids.erase(at(element_ids, first), at(element_ids, first + count));
        connectivity.erase(at(connectivity, 8 * first), at(connectivity, 8 * (first + count)));
    }
};

}  // namespace fem::storage::testing
