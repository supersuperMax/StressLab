#include "host_data.h"

#include "fem/core/extension/error.h"
#include "fem/core/model/element_type.h"
#include "fem/core/model/mesh.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fem::extension::detail {

namespace {

using model::ElementSide;
using model::Index;
using model::Label;
using nlohmann::json;

static_assert(sizeof(ElementSide) == 2 * sizeof(Index), "sides are pairs of int32 in messages");

/// Arrays of more elements are refused: no mesh array is that long (`Index` is int32).
constexpr std::uint64_t max_count = std::uint64_t{1} << 32U;

[[noreturn]] void fail(const std::string& message) {
    throw ExtensionError(ExtensionErrc::command_failed, message);
}

template <class T>
constexpr std::string_view dtype_of() {
    if constexpr (std::is_same_v<T, double>) {
        return "f8";
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        return "i8";
    } else {
        static_assert(std::is_same_v<T, std::int32_t>);
        return "i4";
    }
}

const json& member(const json& object, std::string_view key, std::string_view what) {
    const auto it = object.find(key);
    if (it == object.end()) {
        fail(std::string(what) + " has no '" + std::string(key) + "'");
    }
    return *it;
}

std::uint64_t unsigned_member(const json& object, std::string_view key, std::string_view what,
                              std::uint64_t fallback) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    if (!it->is_number_unsigned() && !(it->is_number_integer() && it->get<std::int64_t>() >= 0)) {
        fail("'" + std::string(key) + "' of " + std::string(what) +
             " must be a non-negative integer");
    }
    return it->get<std::uint64_t>();
}

/// An array of a message: a JSON list, or `{"shm", "offset", "dtype", "count"}`.
template <class T>
std::vector<T> read_array(const json& array, std::string_view what) {
    std::vector<T> values;
    if (array.is_array()) {
        values.reserve(array.size());
        for (const json& item : array) {
            if constexpr (std::is_floating_point_v<T>) {
                if (!item.is_number()) {
                    fail(std::string(what) + " must hold numbers");
                }
                values.push_back(item.get<double>());
            } else {
                if (!item.is_number_integer() && !item.is_number_unsigned()) {
                    fail(std::string(what) + " must hold integers");
                }
                if (item.is_number_unsigned() &&
                    item.get<std::uint64_t>() >
                        static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
                    fail(std::string(what) + " holds an integer out of range");
                }
                const auto value = item.get<std::int64_t>();
                if (value < std::numeric_limits<T>::min() ||
                    value > std::numeric_limits<T>::max()) {
                    fail(std::string(what) + " holds an integer out of range");
                }
                values.push_back(static_cast<T>(value));
            }
        }
        return values;
    }
    if (!array.is_object()) {
        fail(std::string(what) + " must be a list or an array in shared memory");
    }
    const json& dtype = member(array, "dtype", what);
    if (!dtype.is_string() || dtype.get<std::string>() != dtype_of<T>()) {
        fail(std::string(what) + " must have dtype '" + std::string(dtype_of<T>()) + "'");
    }
    const std::uint64_t count = unsigned_member(array, "count", what, 0);
    const std::uint64_t offset = unsigned_member(array, "offset", what, 0);
    if (count > max_count) {
        fail(std::string(what) + " is too long");
    }
    const json& name = member(array, "shm", what);
    if (!name.is_string()) {
        fail("'shm' of " + std::string(what) + " must be a name");
    }
    values.resize(static_cast<std::size_t>(count));
    if (count == 0) {
        return values;
    }
    const std::size_t bytes = values.size() * sizeof(T);
    try {
        const ipc::SharedMemory memory = ipc::SharedMemory::open(
            name.get<std::string>(), static_cast<std::size_t>(offset) + bytes);
        std::memcpy(values.data(), memory.data() + offset, bytes);
    } catch (const ipc::SharedMemoryError& e) {
        fail(std::string(what) + ": " + e.what());
    }
    return values;
}

const json& object_member(const json& object, std::string_view key, std::string_view what) {
    const json& value = member(object, key, what);
    if (!value.is_object()) {
        fail("'" + std::string(key) + "' of " + std::string(what) + " must be an object");
    }
    return value;
}

std::shared_ptr<const model::Mesh> mesh_from_json(const json& data) {
    const json& nodes = object_member(data, "nodes", "the mesh");
    model::NodeArrays arrays;
    arrays.ids = read_array<Label>(member(nodes, "ids", "the nodes"), "node ids");
    arrays.x = read_array<double>(member(nodes, "x", "the nodes"), "x");
    arrays.y = read_array<double>(member(nodes, "y", "the nodes"), "y");
    arrays.z = read_array<double>(member(nodes, "z", "the nodes"), "z");
    std::vector<model::ElementBlock> blocks;
    if (const auto list = data.find("blocks"); list != data.end()) {
        if (!list->is_array()) {
            fail("'blocks' of the mesh must be a list");
        }
        for (const json& block : *list) {
            if (!block.is_object()) {
                fail("a block of the mesh must be an object");
            }
            const json& type = member(block, "type", "a block");
            const std::optional<model::ElementType> parsed =
                type.is_string() ? model::parse_element_type(type.get<std::string>())
                                 : std::nullopt;
            if (!parsed) {
                fail("unknown element type " + type.dump());
            }
            blocks.emplace_back(
                *parsed, read_array<Label>(member(block, "ids", "a block"), "element ids"),
                read_array<Index>(member(block, "connectivity", "a block"), "connectivity"));
        }
    }
    return std::make_shared<const model::Mesh>(std::move(arrays), std::move(blocks));
}

/// Arrays of data for a module, laid out one after another in shared memory, or inline.
class ArrayWriter {
public:
    template <class T>
    json add(std::span<const T> values) {
        const std::size_t index = arrays_.size();
        arrays_.push_back({values.data(), values.size_bytes(), std::string(dtype_of<T>()),
                           values.size(), [values] {
                               json list = json::array();
                               for (const T value : values) {
                                   list.push_back(value);
                               }
                               return list;
                           }});
        bytes_ += (values.size_bytes() + 7U) & ~std::size_t{7U};
        return json(index);  // Placeholder, replaced by `finish`.
    }

    /// Replaces the placeholders in `data` by the arrays.
    void finish(json& data, std::optional<std::size_t> threshold,
                std::optional<ipc::SharedMemory>& segment) {
        std::vector<json> placed(arrays_.size());
        if (threshold && bytes_ >= *threshold && bytes_ > 0) {
            ipc::SharedMemory memory = ipc::SharedMemory::create(bytes_);
            std::size_t offset = 0;
            for (std::size_t i = 0; i < arrays_.size(); ++i) {
                const Array& array = arrays_[i];
                if (array.bytes > 0) {
                    std::memcpy(memory.data() + offset, array.data, array.bytes);
                }
                placed[i] = {{"shm", memory.name()},
                             {"offset", offset},
                             {"dtype", array.dtype},
                             {"count", array.count}};
                offset += (array.bytes + 7U) & ~std::size_t{7U};
            }
            segment = std::move(memory);
        } else {
            for (std::size_t i = 0; i < arrays_.size(); ++i) {
                placed[i] = arrays_[i].inline_list();
            }
        }
        replace(data, placed);
    }

private:
    struct Array {
        const void* data;
        std::size_t bytes;
        std::string dtype;
        std::size_t count;
        std::function<json()> inline_list;
    };

    /// Placeholders are the only plain integers in the data written here.
    static void replace(json& node, const std::vector<json>& placed) {
        if (node.is_number_unsigned() || node.is_number_integer()) {
            node = placed[node.get<std::size_t>()];
        } else if (node.is_object() || node.is_array()) {
            for (json& child : node) {
                replace(child, placed);
            }
        }
    }

    std::vector<Array> arrays_;
    std::size_t bytes_ = 0;
};

}  // namespace

std::shared_ptr<const model::ObjectData> data_from_json(const json& json_data) {
    if (!json_data.is_object()) {
        fail("data must be an object with a 'kind'");
    }
    const json& kind = member(json_data, "kind", "data");
    const std::string name = kind.is_string() ? kind.get<std::string>() : kind.dump();
    if (name == "mesh") {
        return mesh_from_json(json_data);
    }
    if (name == "nodeSet") {
        return std::make_shared<const model::NodeSet>(
            read_array<Index>(member(json_data, "nodes", "the node set"), "nodes"));
    }
    if (name == "elementSet") {
        return std::make_shared<const model::ElementSet>(
            read_array<Index>(member(json_data, "elements", "the element set"), "elements"));
    }
    if (name == "surface") {
        const std::vector<Index> flat =
            read_array<Index>(member(json_data, "sides", "the surface"), "sides");
        if (flat.size() % 2 != 0) {
            fail("sides of a surface are pairs (element, side)");
        }
        std::vector<ElementSide> sides(flat.size() / 2);
        for (std::size_t i = 0; i < sides.size(); ++i) {
            sides[i] = ElementSide{flat[2 * i], flat[2 * i + 1]};
        }
        return std::make_shared<const model::Surface>(std::move(sides));
    }
    fail("unknown kind of data '" + name + "'");
}

json data_to_json(const model::ObjectData& data, std::optional<std::size_t> threshold,
                  std::optional<ipc::SharedMemory>& segment) {
    ArrayWriter writer;
    json result;
    if (const auto* mesh = dynamic_cast<const model::Mesh*>(&data)) {
        json blocks = json::array();
        for (const model::ElementBlock& block : mesh->blocks()) {
            blocks.push_back({{"type", model::to_string(block.type())},
                              {"ids", writer.add(block.ids())},
                              {"connectivity", writer.add(block.connectivity())}});
        }
        result = {{"kind", "mesh"},
                  {"nodes",
                   {{"ids", writer.add(mesh->node_ids())},
                    {"x", writer.add(mesh->x())},
                    {"y", writer.add(mesh->y())},
                    {"z", writer.add(mesh->z())}}},
                  {"blocks", std::move(blocks)}};
    } else if (const auto* nodes = dynamic_cast<const model::NodeSet*>(&data)) {
        result = {{"kind", "nodeSet"}, {"nodes", writer.add(nodes->nodes())}};
    } else if (const auto* elements = dynamic_cast<const model::ElementSet*>(&data)) {
        result = {{"kind", "elementSet"}, {"elements", writer.add(elements->elements())}};
    } else if (const auto* surface = dynamic_cast<const model::Surface*>(&data)) {
        const std::span<const ElementSide> sides = surface->sides();
        const std::span<const Index> flat(reinterpret_cast<const Index*>(sides.data()),
                                          sides.size() * 2);
        result = {{"kind", "surface"}, {"sides", writer.add(flat)}};
    } else {
        return nullptr;
    }
    writer.finish(result, threshold, segment);
    return result;
}

json object_to_json(const model::Document& document, const model::Object& object) {
    json children = json::array();
    for (const model::ObjectId child : object.children()) {
        children.push_back(child.value());
    }
    json dependencies = json::array();
    for (const model::ObjectId target : object.dependencies()) {
        dependencies.push_back(target.value());
    }
    json data_kind = nullptr;
    if (const auto& data = object.data()) {
        if (dynamic_cast<const model::Mesh*>(data.get()) != nullptr) {
            data_kind = "mesh";
        } else if (dynamic_cast<const model::NodeSet*>(data.get()) != nullptr) {
            data_kind = "nodeSet";
        } else if (dynamic_cast<const model::ElementSet*>(data.get()) != nullptr) {
            data_kind = "elementSet";
        } else if (dynamic_cast<const model::Surface*>(data.get()) != nullptr) {
            data_kind = "surface";
        }
    }
    return {{"id", object.id().value()},
            {"type", object.type()},
            {"owner", object.owner()},
            {"name", object.name()},
            {"schemaVersion", object.schema_version()},
            {"properties", object.properties()},
            {"parent", object.parent() ? json(object.parent().value()) : json(nullptr)},
            {"children", std::move(children)},
            {"dependencies", std::move(dependencies)},
            {"opaque", document.is_opaque(object)},
            {"data", std::move(data_kind)}};
}

}  // namespace fem::extension::detail
