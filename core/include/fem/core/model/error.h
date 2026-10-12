#pragma once

#include "fem/core/model/object_id.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace fem::model {

/// What went wrong in a call to the model API. Lets callers (Command Bus, Python bindings) react
/// to the kind of error without parsing the message.
enum class ModelErrc {
    unknown_object,      ///< No object with this id in the document.
    opaque_object,       ///< The object belongs to a disabled module and cannot be changed.
    invalid_argument,    ///< Empty type or owner, zero schema version and similar.
    invalid_name,        ///< The name is not valid UTF-8.
    duplicate_name,      ///< A sibling of the same type already has this name.
    invalid_properties,  ///< Properties are not a JSON object of finite numbers and UTF-8 text.
    invalid_dependency,  ///< Dependency on itself or on an object that does not exist.
    dependency_cycle,    ///< The dependency would close a cycle.
    invalid_mesh,        ///< Mesh arrays are inconsistent or reference missing nodes.
    invalid_data,        ///< Object data does not match its type, parent or mesh.
    removal_blocked,     ///< Removing would lose or break an object of a disabled module.
    outside_command,     ///< The document was edited outside a running command.
};

[[nodiscard]] std::string_view to_string(ModelErrc code) noexcept;

/// Error raised by the model API for invalid requests. The message is meant for developers and
/// logs; text shown to users is produced from `code()` through localization.
class ModelError : public std::runtime_error {
public:
    ModelError(ModelErrc code, const std::string& message, ObjectId object = {});

    [[nodiscard]] ModelErrc code() const noexcept { return code_; }
    /// The object the error is about, if any.
    [[nodiscard]] ObjectId object() const noexcept { return object_; }

private:
    ModelErrc code_;
    ObjectId object_;
};

}  // namespace fem::model
