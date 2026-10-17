#pragma once

#include "fem/core/extension/registry.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/object_id.h"
#include "fem/core/model/units.h"
#include "fem/storage/project_format.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Comparing two states of a project in the terms of the model (task 2.4, docs/architecture.md §3.1
// "Comparing variants", docs/interface-design.md §12, docs/adr/0020-model-diff.md): which objects
// were added, removed or changed, and in what fields: "Section skin-upper: Thickness 2 → 3 mm;
// Added: Load gust-2; Mesh main: rebuilt (48312 → 61007 elements)".
//
// A diff is made from the files of the two states (`ProjectFiles`, a version of the history or
// the model now) and reads no block of bulk data: an object file describes its data (number of
// nodes and elements) and refs/blobs.json tells whether the block changed. So objects of modules
// that are disabled or not installed are compared like any others, as the JSON they are.
namespace fem::storage {

enum class ChangeKind : std::uint8_t {
    added,    ///< Only in the later state.
    removed,  ///< Only in the earlier state.
    changed,  ///< In both, with something different.
};

/// "added", "removed", "changed".
[[nodiscard]] std::string_view to_string(ChangeKind kind) noexcept;

/// A property that differs: a leaf of the properties of an object. Objects of JSON are compared
/// member by member, everything else (lists, numbers, text) as a whole; 1 and 1.0 differ.
struct PropertyChange {
    /// The path from the root as a JSON Pointer without the leading '/' ("solver/tolerance"), as
    /// forms and `model.set_property` name properties.
    std::string pointer{};
    std::optional<nlohmann::json> before{};  ///< None if the property is new.
    std::optional<nlohmann::json> after{};   ///< None if the property is gone.
};

/// An object that differs between the two states.
struct ObjectChange {
    ChangeKind kind = ChangeKind::changed;
    model::ObjectId id{};
    /// The object in the earlier state; none if it was added.
    std::optional<ObjectRecord> before;
    /// The object in the later state; none if it was removed.
    std::optional<ObjectRecord> after;

    // What differs, for a changed object.
    bool type = false;            ///< Its owner or type (only in edited files: ids are not reused).
    bool name = false;            ///< It was renamed.
    bool parent = false;          ///< It moved in the model tree.
    bool schema_version = false;  ///< It was written with another version of its schema.
    /// Its bulk data: another block, or data appeared or disappeared.
    bool data = false;
    std::vector<PropertyChange> properties;  ///< In the order of their pointers.
    std::vector<model::ObjectId> dependencies_added;
    std::vector<model::ObjectId> dependencies_removed;

    /// The object as it is in the later state, or in the earlier one if it was removed.
    [[nodiscard]] const ObjectRecord& current() const { return after ? *after : *before; }
};

/// The owner, type and name of an object, to name it in a diff.
struct ObjectLabel {
    std::string owner;
    std::string type;
    std::string name;
};

/// How two states of a model differ.
struct ModelDiff {
    model::UnitSystem units_before{};
    model::UnitSystem units_after{};
    /// Objects that differ, by id.
    std::vector<ObjectChange> objects;
    /// The objects the changes refer to as parents or dependencies, from the later state if it
    /// has them.
    std::map<model::ObjectId, ObjectLabel> referenced;

    [[nodiscard]] bool units_changed() const noexcept { return units_before != units_after; }
    /// The two states have the same model (the versions of modules and the next object id are
    /// not part of it).
    [[nodiscard]] bool empty() const noexcept { return !units_changed() && objects.empty(); }
    /// The change of an object, or nullptr if it did not change.
    [[nodiscard]] const ObjectChange* find(model::ObjectId id) const noexcept;
};

/// How the model of `after` differs from that of `before`. Throws as `read_project_records`
/// does for files that are not a project.
[[nodiscard]] ModelDiff diff_files(const ProjectFiles& before, const ProjectFiles& after);

/// The diff as JSON, for scripts and golden tests: deterministic, objects by id, properties by
/// pointer.
[[nodiscard]] nlohmann::json to_json(const ModelDiff& diff);

/// What `describe` names things with.
struct DiffContext {
    /// Strings of the core ("diff.*", units) and titles of the modules ("%key%").
    const l10n::Localization& localization;
    /// The enabled modules: titles of object types (`treeNodeTypes`) and of fields
    /// (`propertySchemas`). An object of an owner that is not in it is shown as raw JSON, as its
    /// module is disabled or not installed. Without a registry, objects are named by their types
    /// and properties by their pointers, and none is raw.
    const extension::ContributionRegistry* registry = nullptr;
};

/// The diff in words, a line per change: the units, then the objects by id. Values of fields with
/// a quantity (`x-quantity`) are shown with the unit of the model, values of choices with their
/// titles (`x-enumTitles`).
[[nodiscard]] std::vector<std::string> describe(const ModelDiff& diff, const DiffContext& context);

/// A change as a row of the table of the comparison of versions (task 2.6): what `describe` says
/// of an object, a field at a time, named and shown the same way.
struct DiffRow {
    /// The object; invalid for the units of the model.
    model::ObjectId id{};
    ChangeKind kind = ChangeKind::changed;
    /// The section of the model tree: the owner and type of the object (as it is in the later
    /// state, or in the earlier one if it was removed) and the title of its type; for the units,
    /// the core and "units".
    std::string owner{};
    std::string type{};
    std::string section{};
    /// "Material Д16Т"; empty for the units.
    std::string object{};
    /// The title of the field, "Name", "Parent", "Data"...; "Added" or "Removed" for an object
    /// that is only in one state (then `before` and `after` are empty, but for raw objects).
    std::string field{};
    /// The values with their units; "—" for one that is not there.
    std::string before{};
    std::string after{};
    /// The module of the object is disabled or not installed: the values are its raw JSON.
    bool raw = false;
};

/// The diff as rows: the units, then the objects by id, each changed object with a row per name,
/// parent, property (by pointer), dependency and data that differ.
[[nodiscard]] std::vector<DiffRow> describe_rows(const ModelDiff& diff, const DiffContext& context);

}  // namespace fem::storage
