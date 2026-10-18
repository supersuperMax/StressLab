#include "fem/core/model/mesh.h"

#include "fem/core/model/array_data.h"
#include "fem/core/model/element_type.h"

#include "common.h"
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
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
struct ArrayDataRef {
    std::shared_ptr<const model::ArrayData> data;
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

/// A named array from an array-like object of one or two dimensions, copied: rows of the width of
/// its second dimension. Floats become float64, integers int32 if they are at most 32 bits wide
/// (unsigned: 16) and int64 otherwise; anything else is refused.
model::NamedArray make_named_array(const std::string& name, nb::handle object) {
    const nb::module_ numpy = nb::module_::import_("numpy");
    const nb::object array = numpy.attr("asarray")(object);
    const auto dims = nb::cast<std::size_t>(array.attr("ndim"));
    if (dims != 1 && dims != 2) {
        throw nb::value_error(
            ("array '" + name + "': expected one or two dimensions, got " + std::to_string(dims))
                .c_str());
    }
    const nb::tuple shape = nb::cast<nb::tuple>(array.attr("shape"));
    const std::size_t width = dims == 2 ? nb::cast<std::size_t>(shape[1]) : 1;
    const nb::object dtype = array.attr("dtype");
    const auto kind = nb::cast<std::string>(dtype.attr("kind"));
    const auto bytes = nb::cast<std::size_t>(dtype.attr("itemsize"));
    const std::string what = "array '" + name + "'";
    model::NamedArray::Items items;
    if (kind == "f") {
        items = to_vector<double>(array, what.c_str());
    } else if ((kind == "i" && bytes <= 4) || (kind == "u" && bytes <= 2)) {
        items = to_vector<std::int32_t>(array, what.c_str());
    } else if (kind == "i" || kind == "u") {
        // to_vector checks the range of narrower types only; uint64 would wrap around.
        if (kind == "u" && bytes == 8 &&
            nb::cast<bool>(array.attr("__gt__")(nb::int_(std::numeric_limits<std::int64_t>::max()))
                               .attr("any")()
                               .attr("item")())) {
            throw nb::value_error((what + ": a value is out of the range of int64").c_str());
        }
        items = to_vector<std::int64_t>(array, what.c_str());
    } else {
        throw nb::type_error((what + ": expected integers or numbers, got " +
                              nb::cast<std::string>(dtype.attr("name")))
                                 .c_str());
    }
    return {name, std::move(items), width};
}

std::shared_ptr<const model::ArrayData> make_array_data(const nb::dict& arrays) {
    std::vector<model::NamedArray> list;
    list.reserve(arrays.size());
    for (const auto [key, value] : arrays) {
        if (!nb::isinstance<nb::str>(key)) {
            throw nb::type_error("the names of arrays are text");
        }
        list.push_back(make_named_array(nb::cast<std::string>(key), value));
    }
    return std::make_shared<const model::ArrayData>(std::move(list));
}

/// A read-only view of an array: one dimension for a width of 1, rows otherwise.
nb::object array_view(const std::shared_ptr<const model::ArrayData>& data,
                      const model::NamedArray& array) {
    return std::visit(
        [&](const auto& items) -> nb::object {
            using T = typename std::decay_t<decltype(items)>::value_type;
            if (array.width() == 1) {
                return nb::cast(view<T, 1>(items.data(), {array.rows()}, data),
                                nb::rv_policy::automatic);
            }
            return nb::cast(view<T, 2>(items.data(), {array.rows(), array.width()}, data),
                            nb::rv_policy::automatic);
        },
        array.items());
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
    if (auto arrays = std::dynamic_pointer_cast<const model::ArrayData>(data)) {
        return nb::cast(ArrayDataRef{std::move(arrays)});
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
    if (nb::isinstance<ArrayDataRef>(object)) {
        return nb::cast<const ArrayDataRef&>(object).data;
    }
    throw nb::type_error(
        "bulk data is a stresslab.Mesh, NodeSet, ElementSet, Surface or ArrayData");
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

    nb::class_<ArrayDataRef>(
        m, "ArrayData",
        "Bulk data of an object of a module: named arrays with the same number of rows, such as "
        "the values of a field of results and the nodes they belong to. Immutable; arrays are "
        "read-only numpy views of the data in the model, not copies.")
        .def(
            "__init__",
            [](ArrayDataRef* self, const nb::dict& arrays) {
                new (self) ArrayDataRef{make_array_data(arrays)};
            },
            nb::arg("arrays"),
            "Copies the arrays of `arrays` (name -> array-like of one or two dimensions; the "
            "second is the width of a row). Names are 1 to 64 ASCII letters, digits and '_'. "
            "Floats are kept as float64, integers as int32 if they are at most 32 bits wide and "
            "as int64 otherwise.")
        .def_prop_ro("rows", [](const ArrayDataRef& self) { return self.data->rows(); })
        .def_prop_ro(
            "names",
            [](const ArrayDataRef& self) {
                std::vector<std::string> names;
                for (const model::NamedArray& array : self.data->arrays()) {
                    names.push_back(array.name());
                }
                return names;
            },
            "The names of the arrays, sorted.")
        .def(
            "dtype",
            [](const ArrayDataRef& self, const std::string& name) {
                const model::NamedArray* array = self.data->find(name);
                if (array == nullptr) {
                    throw nb::key_error(name.c_str());
                }
                return std::string(model::to_string(array->type()));
            },
            nb::arg("name"), "The type of the items of an array: int32, int64 or float64.")
        .def("__len__", [](const ArrayDataRef& self) { return self.data->arrays().size(); })
        .def("__contains__",
             [](const ArrayDataRef& self, const std::string& name) {
                 return self.data->find(name) != nullptr;
             })
        .def(
            "__getitem__",
            [](const ArrayDataRef& self, const std::string& name) {
                const model::NamedArray* array = self.data->find(name);
                if (array == nullptr) {
                    throw nb::key_error(name.c_str());
                }
                return array_view(self.data, *array);
            },
            "The array: one dimension if its rows are of width 1, (rows, width) otherwise.")
        .def("__iter__", [](nb::handle self) { return nb::iter(self.attr("names")); })
        .def(
            "__eq__",
            [](const ArrayDataRef& self, nb::handle other) {
                return nb::isinstance<ArrayDataRef>(other) &&
                       nb::cast<const ArrayDataRef&>(other).data == self.data;
            },
            nb::is_operator())
        .def("__hash__",
             [](const ArrayDataRef& self) { return std::hash<const void*>{}(self.data.get()); });
}

}  // namespace fem::python
