#pragma once

#include "fem/core/model/element_type.h"
#include "fem/core/model/object_data.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fem::model {

/// Node or element number as the user and the source file know it. Need not be dense or sorted.
using Label = std::int64_t;

/// 0-based position of a node or element in the mesh arrays. Connectivity and sets use indices,
/// not labels, so lookups are direct array accesses.
using Index = std::int32_t;

/// Mesh nodes in structure-of-arrays form: node `i` has label `ids[i]` and coordinates
/// `(x[i], y[i], z[i])`. All four arrays have the same length.
struct NodeArrays {
    std::vector<Label> ids;
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> z;
};

/// Elements of one type. Element `i` has label `ids()[i]` and nodes
/// `connectivity()[i * n, (i + 1) * n)` with `n = nodes_per_element()`, in neutral numbering.
///
/// Move-only, like `Mesh`, so that the arrays are never copied by accident.
class ElementBlock {
public:
    /// Takes ownership of the arrays without copying them.
    /// Throws ModelError(invalid_mesh) if the connectivity size does not match the element count.
    ElementBlock(ElementType type, std::vector<Label> ids, std::vector<Index> connectivity);

    ElementBlock(ElementBlock&&) noexcept = default;
    ElementBlock& operator=(ElementBlock&&) noexcept = default;
    ElementBlock(const ElementBlock&) = delete;
    ElementBlock& operator=(const ElementBlock&) = delete;
    ~ElementBlock() = default;

    [[nodiscard]] ElementType type() const noexcept { return type_; }
    [[nodiscard]] int nodes_per_element() const noexcept { return nodes_per_element_; }
    /// Number of elements in the block.
    [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }
    [[nodiscard]] std::span<const Label> ids() const noexcept { return ids_; }
    [[nodiscard]] std::span<const Index> connectivity() const noexcept { return connectivity_; }
    /// Nodes of element `element` of this block; `element < size()`.
    [[nodiscard]] std::span<const Index> element_nodes(std::size_t element) const noexcept {
        const auto n = static_cast<std::size_t>(nodes_per_element_);
        return std::span<const Index>(connectivity_).subspan(element * n, n);
    }

private:
    ElementType type_;
    int nodes_per_element_;
    std::vector<Label> ids_;
    std::vector<Index> connectivity_;
};

/// Nodes and elements of a mesh, immutable after construction.
///
/// Elements have global indices: the blocks are numbered one after another in their order, so
/// element `k` of block `b` has global index `block_offset(b) + k`. Element sets and surfaces use
/// global indices.
class Mesh final : public ObjectData {
public:
    /// Takes ownership of the arrays without copying them.
    /// Throws ModelError(invalid_mesh) if the node arrays differ in length, a coordinate is not
    /// finite, a connectivity entry is not a node index, or the mesh exceeds the `Index` range.
    Mesh(NodeArrays nodes, std::vector<ElementBlock> blocks);

    Mesh(Mesh&&) noexcept = default;
    Mesh& operator=(Mesh&&) noexcept = default;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    ~Mesh() override = default;

    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.ids.size(); }
    [[nodiscard]] std::span<const Label> node_ids() const noexcept { return nodes_.ids; }
    [[nodiscard]] std::span<const double> x() const noexcept { return nodes_.x; }
    [[nodiscard]] std::span<const double> y() const noexcept { return nodes_.y; }
    [[nodiscard]] std::span<const double> z() const noexcept { return nodes_.z; }

    [[nodiscard]] std::span<const ElementBlock> blocks() const noexcept { return blocks_; }
    [[nodiscard]] std::size_t element_count() const noexcept { return block_offsets_.back(); }
    /// Global index of the first element of block `block`; `block <= blocks().size()`.
    [[nodiscard]] std::size_t block_offset(std::size_t block) const noexcept {
        return block_offsets_[block];
    }

    struct ElementLocation {
        std::size_t block;
        std::size_t element;  ///< Position inside the block.
    };
    /// Block and position of the element with global index `element`.
    /// Throws ModelError(invalid_data) if there is no such element.
    [[nodiscard]] ElementLocation locate(Index element) const;
    [[nodiscard]] ElementType element_type(Index element) const;

private:
    NodeArrays nodes_;
    std::vector<ElementBlock> blocks_;
    std::vector<std::size_t> block_offsets_;  // blocks_.size() + 1 entries, starts with 0
};

/// Named group of mesh entities (node set, element set, surface). A set is always a child of the
/// mesh object whose indices it uses, and it is checked against that mesh.
class MeshSet : public ObjectData {
public:
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    /// Throws ModelError(invalid_data) if the set refers to entities that `mesh` does not have.
    virtual void check_against(const Mesh& mesh) const = 0;
};

/// Node indices, sorted and without duplicates.
class NodeSet final : public MeshSet {
public:
    /// Sorts the indices and drops duplicates in place, without copying the array.
    /// Throws ModelError(invalid_data) for negative indices.
    explicit NodeSet(std::vector<Index> nodes);

    [[nodiscard]] std::span<const Index> nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::size_t size() const noexcept override { return nodes_.size(); }
    void check_against(const Mesh& mesh) const override;

private:
    std::vector<Index> nodes_;
};

/// Global element indices, sorted and without duplicates.
class ElementSet final : public MeshSet {
public:
    /// Sorts the indices and drops duplicates in place, without copying the array.
    /// Throws ModelError(invalid_data) for negative indices.
    explicit ElementSet(std::vector<Index> elements);

    [[nodiscard]] std::span<const Index> elements() const noexcept { return elements_; }
    [[nodiscard]] std::size_t size() const noexcept override { return elements_.size(); }
    void check_against(const Mesh& mesh) const override;

private:
    std::vector<Index> elements_;
};

/// One side of one element: global element index and side number (see `Side`).
struct ElementSide {
    Index element = 0;
    Index side = 0;

    friend auto operator<=>(const ElementSide&, const ElementSide&) = default;
};

/// Element sides, sorted and without duplicates.
class Surface final : public MeshSet {
public:
    /// Sorts the sides and drops duplicates in place, without copying the array.
    /// Throws ModelError(invalid_data) for negative indices.
    explicit Surface(std::vector<ElementSide> sides);

    [[nodiscard]] std::span<const ElementSide> sides() const noexcept { return sides_; }
    [[nodiscard]] std::size_t size() const noexcept override { return sides_.size(); }
    void check_against(const Mesh& mesh) const override;

private:
    std::vector<ElementSide> sides_;
};

}  // namespace fem::model
