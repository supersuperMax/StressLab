#include "fem/core/model/object.h"

#include <algorithm>
#include <utility>

namespace fem::model {

namespace {

NewObject core_object(std::string_view type, std::string name, ObjectId parent,
                      std::shared_ptr<const ObjectData> data) {
    NewObject init;
    init.type = std::string(type);
    init.owner = std::string(core_owner);
    init.name = std::move(name);
    init.parent = parent;
    init.data = std::move(data);
    return init;
}

}  // namespace

NewObject mesh_object(std::string name, std::shared_ptr<const Mesh> mesh, ObjectId parent) {
    return core_object(core_types::mesh, std::move(name), parent, std::move(mesh));
}

NewObject node_set_object(ObjectId mesh, std::string name, std::shared_ptr<const NodeSet> set) {
    return core_object(core_types::node_set, std::move(name), mesh, std::move(set));
}

NewObject element_set_object(ObjectId mesh, std::string name,
                             std::shared_ptr<const ElementSet> set) {
    return core_object(core_types::element_set, std::move(name), mesh, std::move(set));
}

NewObject surface_object(ObjectId mesh, std::string name, std::shared_ptr<const Surface> surface) {
    return core_object(core_types::surface, std::move(name), mesh, std::move(surface));
}

Object::Object(ObjectId id, NewObject&& init)
    : id_(id),
      type_(std::move(init.type)),
      owner_(std::move(init.owner)),
      schema_version_(init.schema_version),
      name_(std::move(init.name)),
      properties_(std::move(init.properties)),
      parent_(init.parent),
      dependencies_(std::move(init.dependencies)),
      data_(std::move(init.data)) {}

bool Object::depends_on(ObjectId target) const noexcept {
    return std::binary_search(dependencies_.begin(), dependencies_.end(), target);
}

}  // namespace fem::model
