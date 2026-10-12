#include "fem/core/model/mesh.h"

#include "fem/core/model/error.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace fem::model {

namespace {

constexpr auto max_index = static_cast<std::size_t>(std::numeric_limits<Index>::max());

[[noreturn]] void throw_invalid_mesh(const std::string& message) {
    throw ModelError(ModelErrc::invalid_mesh, message);
}

[[noreturn]] void throw_invalid_set(const std::string& message) {
    throw ModelError(ModelErrc::invalid_data, message);
}

bool all_finite(const std::vector<double>& values) {
    return std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); });
}

template <class T>
void sort_unique(std::vector<T>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

}  // namespace

ElementBlock::ElementBlock(ElementType type, std::vector<Label> ids,
                           std::vector<Index> connectivity)
    : type_(type),
      nodes_per_element_(model::nodes_per_element(type)),
      ids_(std::move(ids)),
      connectivity_(std::move(connectivity)) {
    if (connectivity_.size() != ids_.size() * static_cast<std::size_t>(nodes_per_element_)) {
        throw_invalid_mesh("block of " + std::to_string(ids_.size()) + " " +
                           std::string(to_string(type)) + " elements has " +
                           std::to_string(connectivity_.size()) + " connectivity entries");
    }
}

Mesh::Mesh(NodeArrays nodes, std::vector<ElementBlock> blocks)
    : nodes_(std::move(nodes)), blocks_(std::move(blocks)) {
    const std::size_t count = nodes_.ids.size();
    if (nodes_.x.size() != count || nodes_.y.size() != count || nodes_.z.size() != count) {
        throw_invalid_mesh("node arrays differ in length: " + std::to_string(count) + " ids, " +
                           std::to_string(nodes_.x.size()) + " x, " +
                           std::to_string(nodes_.y.size()) + " y, " +
                           std::to_string(nodes_.z.size()) + " z");
    }
    if (count > max_index) {
        throw_invalid_mesh("too many nodes: " + std::to_string(count));
    }
    if (!all_finite(nodes_.x) || !all_finite(nodes_.y) || !all_finite(nodes_.z)) {
        throw_invalid_mesh("node coordinates must be finite");
    }

    const auto node_count = static_cast<Index>(count);
    block_offsets_.reserve(blocks_.size() + 1);
    block_offsets_.push_back(0);
    for (const ElementBlock& block : blocks_) {
        const auto outside =
            std::find_if(block.connectivity().begin(), block.connectivity().end(),
                         [node_count](Index node) { return node < 0 || node >= node_count; });
        if (outside != block.connectivity().end()) {
            throw_invalid_mesh(std::string(to_string(block.type())) +
                               " block refers to node index " + std::to_string(*outside) + " of " +
                               std::to_string(count));
        }
        block_offsets_.push_back(block_offsets_.back() + block.size());
    }
    if (block_offsets_.back() > max_index) {
        throw_invalid_mesh("too many elements: " + std::to_string(block_offsets_.back()));
    }
}

Mesh::ElementLocation Mesh::locate(Index element) const {
    if (element < 0 || static_cast<std::size_t>(element) >= element_count()) {
        throw_invalid_set("no element with index " + std::to_string(element) + " in a mesh of " +
                          std::to_string(element_count()));
    }
    const auto global = static_cast<std::size_t>(element);
    // First offset greater than the element, minus one, is the block that holds it.
    const auto next = std::upper_bound(block_offsets_.begin(), block_offsets_.end(), global);
    const auto block = static_cast<std::size_t>(next - block_offsets_.begin()) - 1;
    return {block, global - block_offsets_[block]};
}

ElementType Mesh::element_type(Index element) const {
    return blocks_[locate(element).block].type();
}

NodeSet::NodeSet(std::vector<Index> nodes) : nodes_(std::move(nodes)) {
    sort_unique(nodes_);
    if (!nodes_.empty() && nodes_.front() < 0) {
        throw_invalid_set("negative node index in a node set");
    }
}

void NodeSet::check_against(const Mesh& mesh) const {
    if (!nodes_.empty() && static_cast<std::size_t>(nodes_.back()) >= mesh.node_count()) {
        throw_invalid_set("node set refers to node index " + std::to_string(nodes_.back()) +
                          " of " + std::to_string(mesh.node_count()));
    }
}

ElementSet::ElementSet(std::vector<Index> elements) : elements_(std::move(elements)) {
    sort_unique(elements_);
    if (!elements_.empty() && elements_.front() < 0) {
        throw_invalid_set("negative element index in an element set");
    }
}

void ElementSet::check_against(const Mesh& mesh) const {
    if (!elements_.empty() && static_cast<std::size_t>(elements_.back()) >= mesh.element_count()) {
        throw_invalid_set("element set refers to element index " +
                          std::to_string(elements_.back()) + " of " +
                          std::to_string(mesh.element_count()));
    }
}

Surface::Surface(std::vector<ElementSide> sides) : sides_(std::move(sides)) {
    sort_unique(sides_);
    const bool negative = std::any_of(sides_.begin(), sides_.end(), [](const ElementSide& s) {
        return s.element < 0 || s.side < 0;
    });
    if (negative) {
        throw_invalid_set("negative element or side index in a surface");
    }
}

void Surface::check_against(const Mesh& mesh) const {
    for (const ElementSide& side : sides_) {
        const ElementType type = mesh.element_type(side.element);
        const std::size_t side_count = element_type_info(type).sides.size();
        if (static_cast<std::size_t>(side.side) >= side_count) {
            throw_invalid_set("surface refers to side " + std::to_string(side.side) + " of " +
                              std::string(to_string(type)) + " element " +
                              std::to_string(side.element) + ", which has " +
                              std::to_string(side_count) + " sides");
        }
    }
}

}  // namespace fem::model
