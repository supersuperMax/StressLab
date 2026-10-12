#pragma once

#include "fem/core/model/mesh.h"
#include "fem/core/model/object_data.h"
#include "fem/core/model/object_id.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fem::model {

/// Editable values of an object: a JSON object with sorted keys. Numbers must be finite and text
/// must be valid UTF-8, so that a later save is deterministic (docs/architecture.md §3.1).
using Properties = nlohmann::json;

/// Owner of the object types defined by the core itself. The core is always enabled.
inline constexpr std::string_view core_owner = "fem.core";

/// Object types defined by the core. Every other type belongs to a module.
namespace core_types {
inline constexpr std::string_view mesh = "mesh";                ///< Data: `Mesh`.
inline constexpr std::string_view node_set = "node_set";        ///< Data: `NodeSet`.
inline constexpr std::string_view element_set = "element_set";  ///< Data: `ElementSet`.
inline constexpr std::string_view surface = "surface";          ///< Data: `Surface`.
}  // namespace core_types

/// Everything needed to create an object; see `Document::create`.
struct NewObject {
    std::string type;   ///< Object type, unique within its owner, e.g. "material".
    std::string owner;  ///< Id of the module that defines the type, e.g. "fem.model.structural".
    std::uint32_t schema_version = 1;
    std::string name;  ///< UTF-8; if not empty, unique among siblings of the same type.
    Properties properties = Properties::object();
    ObjectId parent;  ///< Invalid id for a root object.
    std::vector<ObjectId> dependencies;
    /// Bulk data. Only core types have it for now; a set must be a child of its mesh.
    std::shared_ptr<const ObjectData> data;
};

[[nodiscard]] NewObject mesh_object(std::string name, std::shared_ptr<const Mesh> mesh,
                                    ObjectId parent = {});
[[nodiscard]] NewObject node_set_object(ObjectId mesh, std::string name,
                                        std::shared_ptr<const NodeSet> set);
[[nodiscard]] NewObject element_set_object(ObjectId mesh, std::string name,
                                           std::shared_ptr<const ElementSet> set);
[[nodiscard]] NewObject surface_object(ObjectId mesh, std::string name,
                                       std::shared_ptr<const Surface> surface);

/// An object of the model: a node of the model tree with its properties, bulk data and
/// dependencies on other objects. Objects are created and changed only through `Document`.
class Object {
public:
    Object(Object&&) noexcept = default;
    Object& operator=(Object&&) noexcept = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    ~Object() = default;

    [[nodiscard]] ObjectId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& type() const noexcept { return type_; }
    [[nodiscard]] const std::string& owner() const noexcept { return owner_; }
    [[nodiscard]] std::uint32_t schema_version() const noexcept { return schema_version_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const Properties& properties() const noexcept { return properties_; }

    /// Parent in the model tree; invalid for root objects.
    [[nodiscard]] ObjectId parent() const noexcept { return parent_; }
    /// Children in creation order.
    [[nodiscard]] std::span<const ObjectId> children() const noexcept { return children_; }

    /// Objects this one depends on, sorted. May include removed objects (see `Document::remove`).
    [[nodiscard]] std::span<const ObjectId> dependencies() const noexcept { return dependencies_; }
    [[nodiscard]] bool depends_on(ObjectId target) const noexcept;

    [[nodiscard]] const std::shared_ptr<const ObjectData>& data() const noexcept { return data_; }

private:
    friend class Document;

    Object(ObjectId id, NewObject&& init);

    ObjectId id_;
    std::string type_;
    std::string owner_;
    std::uint32_t schema_version_;
    std::string name_;
    Properties properties_;
    ObjectId parent_;
    std::vector<ObjectId> children_;
    std::vector<ObjectId> dependencies_;
    std::shared_ptr<const ObjectData> data_;
};

}  // namespace fem::model
