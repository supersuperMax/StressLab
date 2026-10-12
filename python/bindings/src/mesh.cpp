#include "fem/core/model/mesh.h"

#include "fem/core/model/element_type.h"

#include "common.h"
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace fem::python {

namespace {

using model::ElementSide;
using model::Index;
using model::Label;

// Python views of bulk data. The data is immutable and shared, so a view stays valid whatever
// happens to the object it came from, and arrays are handed out without copying.
struct MeshRef {
    std::shared_ptr<const model::Mesh> mesh;
};
struct ElementBlockRef {
    std::shared_ptr<const model::Mesh> mesh;
    std::size_t index;

    [[nodiscard]] const model::ElementBlock& get() const { return mesh->blocks()[index]; }
};
struct NodeSetRef {
    std::shared_ptr<const model::NodeSet> set;
};
struct ElementSetRef {
    std::shared_ptr<const model::ElementSet> set;
};
struct SurfaceRef {
    std::shared_ptr<const model::Surface> surface;
};

static_assert(sizeof(ElementSide) == 2 * sizeof(Index) && std::is_standard_layout_v<ElementSide>,
              "a surface is handed to Python as an (n, 2) array of indices");

template <class T, std::size_t Dims>
using ReadOnlyArray = nb::ndarray<nb::numpy, const T, nb::ndim<Dims>, nb::c_contig>;

/// A read-only numpy array over `data`, which `owner` keeps alive. Nothing is copied.
template <class T, std::size_t Dims, class Owner>
ReadOnlyArray<T, Dims> view(const T* data, const std::size_t (&shape)[Dims],
                            std::shared_ptr<const Owner> owner) {
    static const T empty{};
    if (data == nullptr) {
        data = &empty;  // An empty vector may have no storage; numpy wants an address anyway.
    }
    auto* keep = new std::shared_ptr<const Owner>(std::move(owner));
    const nb::capsule capsule(keep, [](void* pointer) noexcept {
        delete static_cast<std::shared_ptr<const Owner>*>(pointer);
    });
    return ReadOnlyArray<T, Dims>(const_cast<T*>(data), Dims, shape, capsule);
}

/// The values of an array-like object as `Target`, copied. Refuses values that would change in
/// the conversion (floats as integers, integers out of range).
template <class Target>
std::vector<Target> to_vector(nb::handle object, const char* what) {
    const nb::module_ numpy = nb::module_::import_("numpy");
    nb::object array = numpy.attr("asarray")(object);
    constexpr bool integral = std::is_integral_v<Target>;
    const char* const wide = integral ? "int64" : "float64";
    if (nb::cast<std::size_t>(array.attr("size")) != 0 &&
        !nb::cast<bool>(numpy.attr("can_cast")(array.attr("dtype"), wide, "same_kind"))) {
        throw nb::type_error((std::string(what) + ": expected " +
                              (integral ? "integers" : "numbers") + ", got " +
                              nb::cast<std::string>(array.attr("dtype").attr("name")))
                                 .c_str());
    }
    array = numpy.attr("ascontiguousarray")(array, nb::arg("dtype") = wide).attr("reshape")(-1);
    using Wide = std::conditional_t<integral, std::int64_t, double>;
    const auto values = nb::cast<nb::ndarray<const Wide, nb::ndim<1>, nb::c_contig>>(array);
    std::vector<Target> result;
    result.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const Wide value = values.data()[i];
        if constexpr (integral && !std::is_same_v<Target, Wide>) {
            if (value < std::numeric_limits<Target>::min() ||
                value > std::numeric_limits<Target>::max()) {
                throw nb::value_error(
                    (std::string(what) + ": " + std::to_string(value) + " is out of range")
                        .c_str());
            }
        }
        result.push_back(static_cast<Target>(value));
    }
    return result;
}

model::ElementType to_element_type(const std::string& name) {
    const std::optional<model::ElementType> type = model::parse_element_type(name);
    if (!type) {
        throw nb::value_error(("unknown element type '" + name + "'").c_str());
    }
    return *type;
}

std::shared_ptr<const model::Mesh> make_mesh(nb::handle node_ids, nb::handle x, nb::handle y,
                                             nb::handle z, nb::handle blocks) {
    model::NodeArrays nodes{to_vector<Label>(node_ids, "node_ids"), to_vector<double>(x, "x"),
                            to_vector<double>(y, "y"), to_vector<double>(z, "z")};
    std::vector<model::ElementBlock> element_blocks;
    for (nb::handle block : blocks) {
        const nb::tuple parts = nb::cast<nb::tuple>(nb::borrow(block));
        if (parts.size() != 3) {
            throw nb::type_error("an element block is (type, ids, connectivity)");
        }
        element_blocks.emplace_back(to_element_type(nb::cast<std::string>(parts[0])),
                                    to_vector<Label>(parts[1], "element ids"),
                                    to_vector<Index>(parts[2], "connectivity"));
    }
    return std::make_shared<const model::Mesh>(std::move(nodes), std::move(element_blocks));
}

}  // namespace

nb::object data_to_python(const std::shared_ptr<const model::ObjectData>& data) {
    if (auto mesh = std::dynamic_pointer_cast<const model::Mesh>(data)) {
        return nb::cast(MeshRef{std::move(mesh)});
    }
    if (auto set = std::dynamic_pointer_cast<const model::NodeSet>(data)) {
        return nb::cast(NodeSetRef{std::move(set)});
    }
    if (auto set = std::dynamic_pointer_cast<const model::ElementSet>(data)) {
        return nb::cast(ElementSetRef{std::move(set)});
    }
    if (auto surface = std::dynamic_pointer_cast<const model::Surface>(data)) {
        return nb::cast(SurfaceRef{std::move(surface)});
    }
    return nb::none();
}

std::shared_ptr<const model::ObjectData> data_from_python(nb::handle object) {
    if (nb::isinstance<MeshRef>(object)) {
        return nb::cast<const MeshRef&>(object).mesh;
    }
    if (nb::isinstance<NodeSetRef>(object)) {
        return nb::cast<const NodeSetRef&>(object).set;
    }
    if (nb::isinstance<ElementSetRef>(object)) {
        return nb::cast<const ElementSetRef&>(object).set;
    }
    if (nb::isinstance<SurfaceRef>(object)) {
        return nb::cast<const SurfaceRef&>(object).surface;
    }
    throw nb::type_error("bulk data is a stresslab.Mesh, NodeSet, ElementSet or Surface");
}

void define_mesh(nb::module_& m) {
    nb::class_<ElementBlockRef>(m, "ElementBlock",
                                "Elements of one type in a mesh. Arrays are read-only views.")
        .def_prop_ro("type",
                     [](const ElementBlockRef& self) {
                         return std::string(model::to_string(self.get().type()));
                     })
        .def_prop_ro("nodes_per_element",
                     [](const ElementBlockRef& self) { return self.get().nodes_per_element(); })
        .def("__len__", [](const ElementBlockRef& self) { return self.get().size(); })
        .def_prop_ro(
            "ids",
            [](const ElementBlockRef& self) {
                const model::ElementBlock& block = self.get();
                return view<Label, 1>(block.ids().data(), {block.size()}, self.mesh);
            },
            nb::rv_policy::automatic)
        .def_prop_ro(
            "connectivity",
            [](const ElementBlockRef& self) {
                const model::ElementBlock& block = self.get();
                const auto columns = static_cast<std::size_t>(block.nodes_per_element());
                return view<Index, 2>(block.connectivity().data(), {block.size(), columns},
                                      self.mesh);
            },
            nb::rv_policy::automatic,
            "Node indices (not labels) of each element, one row per element.");

    nb::class_<MeshRef>(m, "Mesh",
                        "Nodes and elements, immutable. Arrays are read-only numpy views of the "
                        "data in the model, not copies.")
        .def(
            "__init__",
            [](MeshRef* self, nb::handle node_ids, nb::handle x, nb::handle y, nb::handle z,
               nb::handle blocks) { new (self) MeshRef{make_mesh(node_ids, x, y, z, blocks)}; },
            nb::arg("node_ids"), nb::arg("x"), nb::arg("y"), nb::arg("z"),
            nb::arg("blocks") = nb::tuple(),
            "Copies the arrays. `blocks`: (element type, element ids, connectivity) for each "
            "block; connectivity holds node indices, flat or one row per element.")
        .def_prop_ro("node_count", [](const MeshRef& self) { return self.mesh->node_count(); })
        .def_prop_ro("element_count",
                     [](const MeshRef& self) { return self.mesh->element_count(); })
        .def_prop_ro(
            "node_ids",
            [](const MeshRef& self) {
                return view<Label, 1>(self.mesh->node_ids().data(), {self.mesh->node_count()},
                                      self.mesh);
            },
            nb::rv_policy::automatic)
        .def_prop_ro(
            "x",
            [](const MeshRef& self) {
                return view<double, 1>(self.mesh->x().data(), {self.mesh->node_count()}, self.mesh);
            },
            nb::rv_policy::automatic)
        .def_prop_ro(
            "y",
            [](const MeshRef& self) {
                return view<double, 1>(self.mesh->y().data(), {self.mesh->node_count()}, self.mesh);
            },
            nb::rv_policy::automatic)
        .def_prop_ro(
            "z",
            [](const MeshRef& self) {
                return view<double, 1>(self.mesh->z().data(), {self.mesh->node_count()}, self.mesh);
            },
            nb::rv_policy::automatic)
        .def_prop_ro("blocks",
                     [](const MeshRef& self) {
                         nb::list list;
                         for (std::size_t i = 0; i < self.mesh->blocks().size(); ++i) {
                             list.append(ElementBlockRef{self.mesh, i});
                         }
                         return list;
                     })
        .def(
            "block_offset",
            [](const MeshRef& self, std::size_t block) {
                if (block > self.mesh->blocks().size()) {
                    throw nb::index_error("no such block");
                }
                return self.mesh->block_offset(block);
            },
            nb::arg("block"), "Global index of the first element of a block.")
        .def(
            "__eq__",
            [](const MeshRef& self, nb::handle other) {
                return nb::isinstance<MeshRef>(other) &&
                       nb::cast<const MeshRef&>(other).mesh == self.mesh;
            },
            nb::is_operator())
        .def("__hash__",
             [](const MeshRef& self) { return std::hash<const void*>{}(self.mesh.get()); });

    nb::class_<NodeSetRef>(m, "NodeSet", "Node indices of a mesh, sorted, without duplicates.")
        .def(
            "__init__",
            [](NodeSetRef* self, nb::handle nodes) {
                new (self) NodeSetRef{
                    std::make_shared<const model::NodeSet>(to_vector<Index>(nodes, "nodes"))};
            },
            nb::arg("nodes"))
        .def("__len__", [](const NodeSetRef& self) { return self.set->size(); })
        .def_prop_ro(
            "nodes",
            [](const NodeSetRef& self) {
                return view<Index, 1>(self.set->nodes().data(), {self.set->size()}, self.set);
            },
            nb::rv_policy::automatic);

    nb::class_<ElementSetRef>(m, "ElementSet",
                              "Global element indices of a mesh, sorted, without duplicates.")
        .def(
            "__init__",
            [](ElementSetRef* self, nb::handle elements) {
                new (self) ElementSetRef{std::make_shared<const model::ElementSet>(
                    to_vector<Index>(elements, "elements"))};
            },
            nb::arg("elements"))
        .def("__len__", [](const ElementSetRef& self) { return self.set->size(); })
        .def_prop_ro(
            "elements",
            [](const ElementSetRef& self) {
                return view<Index, 1>(self.set->elements().data(), {self.set->size()}, self.set);
            },
            nb::rv_policy::automatic);

    nb::class_<SurfaceRef>(m, "Surface",
                           "Element sides: rows of (global element index, side number).")
        .def(
            "__init__",
            [](SurfaceRef* self, nb::handle sides) {
                const std::vector<Index> flat = to_vector<Index>(sides, "sides");
                if (flat.size() % 2 != 0) {
                    throw nb::value_error("sides: expected pairs of (element, side)");
                }
                std::vector<ElementSide> pairs;
                pairs.reserve(flat.size() / 2);
                for (std::size_t i = 0; i < flat.size(); i += 2) {
                    pairs.push_back({flat[i], flat[i + 1]});
                }
                new (self) SurfaceRef{std::make_shared<const model::Surface>(std::move(pairs))};
            },
            nb::arg("sides"))
        .def("__len__", [](const SurfaceRef& self) { return self.surface->size(); })
        .def_prop_ro(
            "sides",
            [](const SurfaceRef& self) {
                const auto* data = reinterpret_cast<const Index*>(self.surface->sides().data());
                return view<Index, 2>(data, {self.surface->size(), std::size_t{2}}, self.surface);
            },
            nb::rv_policy::automatic);
}

}  // namespace fem::python
