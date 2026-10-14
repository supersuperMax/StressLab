#pragma once

#include <string_view>

// Commands of the core, owned by "fem.core" and registered by every CommandBus. They change the
// model in terms of the Model IR, for scripts, the model tree and the property panel; commands
// that know what objects mean (materials, loads, meshers) belong to modules.
//
// Bulk data (meshes, sets) is not set by these commands: it does not fit into the journal, so it
// is created by module commands that read it from files or compute it again when replayed.
namespace fem::command::model_commands {

/// Creates an object; returns its id.
/// type: string, owner: string, name: string = "", parent: object (optional, root if omitted),
/// dependencies: list of object ids = [], schema_version: integer = 1, properties: dict = {}.
inline constexpr std::string_view create = "model.create";
/// object: object, name: string.
inline constexpr std::string_view rename = "model.rename";
/// Replaces all properties. object: object, properties: dict.
inline constexpr std::string_view set_properties = "model.set_properties";
/// Sets one property; the others stay. object: object, key: string, value: any. `key` is the name
/// of a property, or a path into nested objects written as a JSON Pointer without the leading '/'
/// ("solver/tolerance"; '~1' is '/' and '~0' is '~' in a name); missing objects on the path are
/// created. A value of None removes the property.
inline constexpr std::string_view set_property = "model.set_property";
/// Removes an object with its subtree; returns the removed ids. Throws ModelError(removal_blocked)
/// if that would lose or break an object of a disabled module. object: object.
inline constexpr std::string_view remove = "model.remove";
/// Returns false if the dependency existed already. object: object, target: object.
inline constexpr std::string_view add_dependency = "model.add_dependency";
/// Returns false if there was no such dependency. object: object, target: object.
inline constexpr std::string_view remove_dependency = "model.remove_dependency";
/// Declares the unit system; values are not converted. length, mass, time, temperature: strings
/// with the unit symbols of units.h ("mm", "t", "s", "degC").
inline constexpr std::string_view set_units = "model.set_units";

}  // namespace fem::command::model_commands
