#pragma once

#include "fem/core/model/object.h"
#include "fem/core/model/object_id.h"
#include "fem/core/model/units.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace fem::model {

/// Why an object cannot be removed. Text for the user is built from these fields through
/// localization; the owner and type of the objects are taken from the document.
struct RemovalBlocker {
    enum class Reason : std::uint8_t {
        opaque,              ///< `object` belongs to a disabled module and would be lost.
        required_by_opaque,  ///< `opaque` belongs to a disabled module and depends on `object`.
    };

    Reason reason;
    ObjectId object;  ///< Object of the removed subtree.
    ObjectId opaque;  ///< Object of a disabled module that would be lost or broken.

    friend bool operator==(const RemovalBlocker&, const RemovalBlocker&) = default;
};

struct RemoveResult {
    std::vector<ObjectId> removed;         ///< Removed subtree in pre-order; empty if blocked.
    std::vector<RemovalBlocker> blockers;  ///< Why nothing was removed; empty on success.

    [[nodiscard]] bool ok() const noexcept { return blockers.empty(); }
};

/// The model: a tree of objects with dependencies, and the unit system the values are in.
///
/// Objects of a module that is not enabled in the document are *opaque*: they stay in the document
/// unchanged and cannot be edited or removed, and nothing they depend on can be removed
/// (docs/architecture.md §4.6, item 4). Objects of the core are never opaque.
///
/// Not thread-safe: the model is changed from one thread (through the Command Bus, task 1.3).
/// Object data is immutable and may be read from any thread.
class Document {
public:
    explicit Document(UnitSystem units = UnitSystem::si());

    Document(Document&&) = default;
    Document& operator=(Document&&) = default;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    ~Document() = default;

    // --- Units -----------------------------------------------------------------------------------

    [[nodiscard]] const UnitSystem& units() const noexcept { return units_; }
    /// Declares the unit system of the model. Values are not converted.
    void set_units(const UnitSystem& units) noexcept { units_ = units; }

    // --- Modules ---------------------------------------------------------------------------------

    /// Enables or disables the objects of a module. Enabling is not a change of the model.
    /// Throws ModelError(invalid_argument) for an empty owner or for the core.
    void set_owner_enabled(std::string_view owner, bool enabled);
    [[nodiscard]] bool is_owner_enabled(std::string_view owner) const;
    [[nodiscard]] bool is_opaque(const Object& object) const;
    [[nodiscard]] bool is_opaque(ObjectId id) const;
    /// Owners of the objects in the document, sorted: the modules the model needs.
    [[nodiscard]] std::vector<std::string> owners() const;

    // --- Lookup ----------------------------------------------------------------------------------

    [[nodiscard]] std::size_t size() const noexcept { return objects_.size(); }
    [[nodiscard]] bool contains(ObjectId id) const { return objects_.contains(id); }
    /// The object, or nullptr if there is none with this id.
    [[nodiscard]] const Object* find(ObjectId id) const;
    /// The object; throws ModelError(unknown_object) if there is none with this id.
    [[nodiscard]] const Object& get(ObjectId id) const;
    /// All objects in id order, which is creation order.
    [[nodiscard]] auto objects() const { return std::views::values(objects_); }
    /// Root objects in creation order.
    [[nodiscard]] std::span<const ObjectId> roots() const noexcept { return roots_; }

    [[nodiscard]] std::vector<ObjectId> find_by_type(std::string_view type) const;
    [[nodiscard]] std::vector<ObjectId> find_by_owner(std::string_view owner) const;
    /// Child of `parent` (or root object, if `parent` is invalid) with the given type and name;
    /// invalid id if there is none.
    [[nodiscard]] ObjectId find_child(ObjectId parent, std::string_view type,
                                      std::string_view name) const;

    /// Objects that depend on `id`, sorted. Works for removed objects too.
    [[nodiscard]] std::vector<ObjectId> dependents(ObjectId id) const;
    /// Dependencies of `id` on objects that have been removed.
    [[nodiscard]] std::vector<ObjectId> missing_dependencies(ObjectId id) const;

    /// Bulk data of `id` as `T`. Throws ModelError(invalid_data) if the object has other data.
    template <class T>
    [[nodiscard]] std::shared_ptr<const T> data(ObjectId id) const;

    // --- Changes ---------------------------------------------------------------------------------
    // Task 1.3 routes these through the Command Bus. All of them throw ModelError and leave the
    // document unchanged if the request is invalid.

    /// Creates an object and returns its new id. The object may belong to a disabled module
    /// (this is how a model with such objects is loaded), but its parent must not be opaque.
    ObjectId create(NewObject init);
    void rename(ObjectId id, std::string name);
    void set_properties(ObjectId id, Properties properties);
    /// Replaces the bulk data. Replacing a mesh checks the sets under it against the new mesh.
    void set_data(ObjectId id, std::shared_ptr<const ObjectData> data);

    /// Returns false if the dependency already exists. Refuses cycles and missing targets.
    bool add_dependency(ObjectId dependent, ObjectId target);
    /// Returns false if there was no such dependency.
    bool remove_dependency(ObjectId dependent, ObjectId target);

    /// What prevents removing `id` with its subtree; empty if it can be removed.
    [[nodiscard]] std::vector<RemovalBlocker> removal_blockers(ObjectId id) const;
    /// Removes `id` with its subtree, unless that would lose or break an opaque object; then
    /// nothing changes and the result lists the reasons. Active objects that depend on a removed
    /// object keep the dependency, which becomes missing (see `missing_dependencies`).
    [[nodiscard]] RemoveResult remove(ObjectId id);

private:
    using NameKey = std::tuple<ObjectId, std::string, std::string>;  // parent, type, name

    Object& get_mutable(ObjectId id);
    Object& get_editable(ObjectId id);
    void check_name(const Object* self, ObjectId parent, const std::string& type,
                    const std::string& name) const;
    void check_data(const std::string& owner, const std::string& type, ObjectId parent,
                    const std::shared_ptr<const ObjectData>& data) const;
    [[nodiscard]] bool reaches(ObjectId from, ObjectId to) const;
    [[nodiscard]] std::vector<ObjectId> subtree(ObjectId id) const;
    [[noreturn]] static void throw_wrong_data(ObjectId id);

    UnitSystem units_;
    std::uint64_t next_id_ = 1;
    std::map<ObjectId, Object> objects_;
    std::vector<ObjectId> roots_;
    std::map<ObjectId, std::set<ObjectId>> dependents_;  // target -> dependents; target may be gone
    std::map<NameKey, ObjectId> names_;
    std::set<std::string, std::less<>> enabled_owners_;
};

template <class T>
std::shared_ptr<const T> Document::data(ObjectId id) const {
    auto typed = std::dynamic_pointer_cast<const T>(get(id).data());
    if (!typed) {
        throw_wrong_data(id);
    }
    return typed;
}

}  // namespace fem::model
