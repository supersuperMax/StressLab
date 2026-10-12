#include "fem/core/model/document.h"

#include "fem/core/model/error.h"

#include "utf8.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace fem::model {

namespace {

bool is_core_type(std::string_view type) {
    return type == core_types::mesh || type == core_types::node_set ||
           type == core_types::element_set || type == core_types::surface;
}

[[noreturn]] void throw_invalid_properties(const std::string& message) {
    throw ModelError(ModelErrc::invalid_properties, message);
}

void check_property_value(const Properties& value) {
    switch (value.type()) {
        case Properties::value_t::object:
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!detail::is_valid_utf8(it.key())) {
                    throw_invalid_properties("property key is not valid UTF-8");
                }
                check_property_value(it.value());
            }
            break;
        case Properties::value_t::array:
            for (const Properties& item : value) {
                check_property_value(item);
            }
            break;
        case Properties::value_t::string:
            if (!detail::is_valid_utf8(value.get_ref<const std::string&>())) {
                throw_invalid_properties("property text is not valid UTF-8");
            }
            break;
        case Properties::value_t::number_float:
            if (!std::isfinite(value.get<double>())) {
                throw_invalid_properties("property numbers must be finite");
            }
            break;
        case Properties::value_t::null:
        case Properties::value_t::boolean:
        case Properties::value_t::number_integer:
        case Properties::value_t::number_unsigned:
            break;
        case Properties::value_t::binary:
        case Properties::value_t::discarded:
            throw_invalid_properties("properties must not hold binary or discarded values");
    }
}

void check_properties(const Properties& properties) {
    if (!properties.is_object()) {
        throw_invalid_properties("properties must be a JSON object");
    }
    check_property_value(properties);
}

void erase_dependent(std::map<ObjectId, std::set<ObjectId>>& index, ObjectId target,
                     ObjectId dependent) {
    const auto entry = index.find(target);
    if (entry == index.end()) {
        return;
    }
    entry->second.erase(dependent);
    if (entry->second.empty()) {
        index.erase(entry);
    }
}

}  // namespace

Document::Document(UnitSystem units) : units_(units) {}

// --- Modules -------------------------------------------------------------------------------------

void Document::set_owner_enabled(std::string_view owner, bool enabled) {
    if (owner.empty() || owner == core_owner) {
        throw ModelError(
            ModelErrc::invalid_argument,
            "only modules can be enabled or disabled, not '" + std::string(owner) + "'");
    }
    if (enabled) {
        enabled_owners_.emplace(owner);
    } else if (const auto it = enabled_owners_.find(owner); it != enabled_owners_.end()) {
        enabled_owners_.erase(it);
    }
}

bool Document::is_owner_enabled(std::string_view owner) const {
    return owner == core_owner || enabled_owners_.contains(owner);
}

bool Document::is_opaque(const Object& object) const {
    return !is_owner_enabled(object.owner());
}

bool Document::is_opaque(ObjectId id) const {
    return is_opaque(get(id));
}

std::vector<std::string> Document::owners() const {
    std::set<std::string, std::less<>> owners;
    for (const auto& [id, object] : objects_) {
        owners.insert(object.owner());
    }
    return {owners.begin(), owners.end()};
}

// --- Lookup --------------------------------------------------------------------------------------

const Object* Document::find(ObjectId id) const {
    const auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}

const Object& Document::get(ObjectId id) const {
    if (const Object* object = find(id)) {
        return *object;
    }
    throw ModelError(ModelErrc::unknown_object, "no such object", id);
}

std::vector<ObjectId> Document::find_by_type(std::string_view type) const {
    std::vector<ObjectId> result;
    for (const auto& [id, object] : objects_) {
        if (object.type() == type) {
            result.push_back(id);
        }
    }
    return result;
}

std::vector<ObjectId> Document::find_by_owner(std::string_view owner) const {
    std::vector<ObjectId> result;
    for (const auto& [id, object] : objects_) {
        if (object.owner() == owner) {
            result.push_back(id);
        }
    }
    return result;
}

ObjectId Document::find_child(ObjectId parent, std::string_view type, std::string_view name) const {
    if (name.empty()) {
        return {};
    }
    const auto it = names_.find(NameKey{parent, std::string(type), std::string(name)});
    return it == names_.end() ? ObjectId{} : it->second;
}

std::vector<ObjectId> Document::dependents(ObjectId id) const {
    const auto it = dependents_.find(id);
    if (it == dependents_.end()) {
        return {};
    }
    return {it->second.begin(), it->second.end()};
}

std::vector<ObjectId> Document::missing_dependencies(ObjectId id) const {
    std::vector<ObjectId> result;
    for (const ObjectId target : get(id).dependencies()) {
        if (!contains(target)) {
            result.push_back(target);
        }
    }
    return result;
}

void Document::throw_wrong_data(ObjectId id) {
    throw ModelError(ModelErrc::invalid_data, "object has no data of the requested type", id);
}

// --- Changes -------------------------------------------------------------------------------------

ObjectId Document::create(NewObject init) {
    if (init.type.empty() || init.owner.empty()) {
        throw ModelError(ModelErrc::invalid_argument, "object type and owner must not be empty");
    }
    if (init.schema_version == 0) {
        throw ModelError(ModelErrc::invalid_argument, "schema version starts at 1");
    }
    if (init.owner == core_owner && !is_core_type(init.type)) {
        throw ModelError(ModelErrc::invalid_argument,
                         "'" + init.type + "' is not an object type of the core");
    }
    if (init.parent.is_valid() && is_opaque(get(init.parent))) {
        throw ModelError(ModelErrc::opaque_object,
                         "cannot add a child to an object of a disabled module", init.parent);
    }
    check_name(nullptr, init.parent, init.type, init.name);
    check_properties(init.properties);
    std::sort(init.dependencies.begin(), init.dependencies.end());
    init.dependencies.erase(std::unique(init.dependencies.begin(), init.dependencies.end()),
                            init.dependencies.end());
    for (const ObjectId target : init.dependencies) {
        if (!contains(target)) {
            throw ModelError(ModelErrc::invalid_dependency, "dependency on a missing object",
                             target);
        }
    }
    check_data(init.owner, init.type, init.parent, init.data);

    // Validation is over: from here on the id is taken and the document changes.
    const ObjectId id{next_id_++};
    Object& object = objects_.emplace(id, Object(id, std::move(init))).first->second;
    if (object.parent_.is_valid()) {
        get_mutable(object.parent_).children_.push_back(id);
    } else {
        roots_.push_back(id);
    }
    if (!object.name_.empty()) {
        names_.emplace(NameKey{object.parent_, object.type_, object.name_}, id);
    }
    for (const ObjectId target : object.dependencies_) {
        dependents_[target].insert(id);
    }
    return id;
}

void Document::rename(ObjectId id, std::string name) {
    Object& object = get_editable(id);
    if (name == object.name_) {
        return;
    }
    check_name(&object, object.parent_, object.type_, name);
    if (!object.name_.empty()) {
        names_.erase(NameKey{object.parent_, object.type_, object.name_});
    }
    object.name_ = std::move(name);
    if (!object.name_.empty()) {
        names_.emplace(NameKey{object.parent_, object.type_, object.name_}, id);
    }
}

void Document::set_properties(ObjectId id, Properties properties) {
    Object& object = get_editable(id);
    check_properties(properties);
    object.properties_ = std::move(properties);
}

void Document::set_data(ObjectId id, std::shared_ptr<const ObjectData> data) {
    Object& object = get_editable(id);
    check_data(object.owner_, object.type_, object.parent_, data);
    if (const auto* mesh = dynamic_cast<const Mesh*>(data.get())) {
        for (const ObjectId child : object.children_) {
            const auto* set = dynamic_cast<const MeshSet*>(get(child).data().get());
            if (set == nullptr) {
                continue;
            }
            try {
                set->check_against(*mesh);
            } catch (const ModelError& error) {
                throw ModelError(ModelErrc::invalid_data,
                                 std::string("set does not fit the new mesh: ") + error.what(),
                                 child);
            }
        }
    }
    object.data_ = std::move(data);
}

bool Document::add_dependency(ObjectId dependent, ObjectId target) {
    Object& object = get_editable(dependent);
    if (target == dependent || !contains(target)) {
        throw ModelError(ModelErrc::invalid_dependency,
                         "dependency on itself or on a missing object " + to_string(target),
                         dependent);
    }
    if (object.depends_on(target)) {
        return false;
    }
    if (reaches(target, dependent)) {
        throw ModelError(ModelErrc::dependency_cycle,
                         "dependency on " + to_string(target) + " would close a cycle", dependent);
    }
    auto& dependencies = object.dependencies_;
    dependencies.insert(std::lower_bound(dependencies.begin(), dependencies.end(), target), target);
    dependents_[target].insert(dependent);
    return true;
}

bool Document::remove_dependency(ObjectId dependent, ObjectId target) {
    Object& object = get_editable(dependent);
    auto& dependencies = object.dependencies_;
    const auto it = std::lower_bound(dependencies.begin(), dependencies.end(), target);
    if (it == dependencies.end() || *it != target) {
        return false;
    }
    dependencies.erase(it);
    erase_dependent(dependents_, target, dependent);
    return true;
}

std::vector<RemovalBlocker> Document::removal_blockers(ObjectId id) const {
    const std::vector<ObjectId> members = subtree(id);
    const std::set<ObjectId> removed(members.begin(), members.end());
    std::vector<RemovalBlocker> blockers;
    for (const ObjectId member : members) {
        if (is_opaque(get(member))) {
            blockers.push_back({RemovalBlocker::Reason::opaque, member, member});
        }
        const auto entry = dependents_.find(member);
        if (entry == dependents_.end()) {
            continue;
        }
        for (const ObjectId dependent : entry->second) {
            if (!removed.contains(dependent) && is_opaque(get(dependent))) {
                blockers.push_back({RemovalBlocker::Reason::required_by_opaque, member, dependent});
            }
        }
    }
    return blockers;
}

RemoveResult Document::remove(ObjectId id) {
    RemoveResult result;
    result.blockers = removal_blockers(id);
    if (!result.ok()) {
        return result;
    }

    result.removed = subtree(id);
    const ObjectId parent = get(id).parent_;
    std::vector<ObjectId>& siblings = parent.is_valid() ? get_mutable(parent).children_ : roots_;
    siblings.erase(std::find(siblings.begin(), siblings.end(), id));

    for (const ObjectId member : result.removed) {
        const Object& object = get(member);
        // Dependencies of removed objects go away; dependencies *on* removed objects stay, so
        // active dependents see them as missing.
        for (const ObjectId target : object.dependencies_) {
            erase_dependent(dependents_, target, member);
        }
        if (!object.name_.empty()) {
            names_.erase(NameKey{object.parent_, object.type_, object.name_});
        }
    }
    for (const ObjectId member : result.removed) {
        objects_.erase(member);
    }
    return result;
}

// --- Helpers -------------------------------------------------------------------------------------

Object& Document::get_mutable(ObjectId id) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) {
        throw ModelError(ModelErrc::unknown_object, "no such object", id);
    }
    return it->second;
}

Object& Document::get_editable(ObjectId id) {
    Object& object = get_mutable(id);
    if (is_opaque(object)) {
        throw ModelError(ModelErrc::opaque_object,
                         "object belongs to disabled module '" + object.owner_ + "'", id);
    }
    return object;
}

void Document::check_name(const Object* self, ObjectId parent, const std::string& type,
                          const std::string& name) const {
    if (!detail::is_valid_utf8(name)) {
        throw ModelError(ModelErrc::invalid_name, "object name is not valid UTF-8",
                         self != nullptr ? self->id() : ObjectId{});
    }
    if (name.empty()) {
        return;
    }
    const auto it = names_.find(NameKey{parent, type, name});
    if (it != names_.end() && (self == nullptr || it->second != self->id())) {
        throw ModelError(ModelErrc::duplicate_name,
                         "a sibling " + type + " is already named '" + name + "'", it->second);
    }
}

void Document::check_data(const std::string& owner, const std::string& type, ObjectId parent,
                          const std::shared_ptr<const ObjectData>& data) const {
    if (owner != core_owner) {
        if (data) {
            throw ModelError(ModelErrc::invalid_data,
                             "only object types of the core carry bulk data so far");
        }
        return;
    }

    const ObjectData* raw = data.get();
    if (type == core_types::mesh) {
        if (dynamic_cast<const Mesh*>(raw) == nullptr) {
            throw ModelError(ModelErrc::invalid_data, "a mesh object needs Mesh data");
        }
        return;
    }

    const bool matches =
        (type == core_types::node_set && dynamic_cast<const NodeSet*>(raw)) ||
        (type == core_types::element_set && dynamic_cast<const ElementSet*>(raw)) ||
        (type == core_types::surface && dynamic_cast<const Surface*>(raw));
    if (!matches) {
        throw ModelError(ModelErrc::invalid_data, "a " + type + " object needs " + type + " data");
    }
    const Object* parent_object = find(parent);
    const auto* mesh =
        parent_object != nullptr ? dynamic_cast<const Mesh*>(parent_object->data().get()) : nullptr;
    if (mesh == nullptr) {
        throw ModelError(ModelErrc::invalid_data, "a " + type + " must be a child of a mesh",
                         parent);
    }
    static_cast<const MeshSet&>(*raw).check_against(*mesh);
}

bool Document::reaches(ObjectId from, ObjectId to) const {
    std::vector<ObjectId> pending{from};
    std::set<ObjectId> visited;
    while (!pending.empty()) {
        const ObjectId current = pending.back();
        pending.pop_back();
        if (current == to) {
            return true;
        }
        if (!visited.insert(current).second) {
            continue;
        }
        if (const Object* object = find(current)) {
            pending.insert(pending.end(), object->dependencies_.begin(),
                           object->dependencies_.end());
        }
    }
    return false;
}

std::vector<ObjectId> Document::subtree(ObjectId id) const {
    std::vector<ObjectId> result;
    std::vector<ObjectId> pending{id};
    while (!pending.empty()) {
        const ObjectId current = pending.back();
        pending.pop_back();
        result.push_back(current);
        const std::vector<ObjectId>& children = get(current).children_;
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return result;
}

}  // namespace fem::model
