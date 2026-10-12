#include "fem/core/model/error.h"

#include "fem/core/model/object_id.h"

#include <ostream>

namespace fem::model {

std::string to_string(ObjectId id) {
    return id.is_valid() ? "#" + std::to_string(id.value()) : std::string("#none");
}

std::ostream& operator<<(std::ostream& out, ObjectId id) {
    return out << to_string(id);
}

std::string_view to_string(ModelErrc code) noexcept {
    switch (code) {
        case ModelErrc::unknown_object:
            return "unknown_object";
        case ModelErrc::opaque_object:
            return "opaque_object";
        case ModelErrc::invalid_argument:
            return "invalid_argument";
        case ModelErrc::invalid_name:
            return "invalid_name";
        case ModelErrc::duplicate_name:
            return "duplicate_name";
        case ModelErrc::invalid_properties:
            return "invalid_properties";
        case ModelErrc::invalid_dependency:
            return "invalid_dependency";
        case ModelErrc::dependency_cycle:
            return "dependency_cycle";
        case ModelErrc::invalid_mesh:
            return "invalid_mesh";
        case ModelErrc::invalid_data:
            return "invalid_data";
    }
    return "unknown";
}

ModelError::ModelError(ModelErrc code, const std::string& message, ObjectId object)
    : std::runtime_error(object.is_valid() ? to_string(object) + ": " + message : message),
      code_(code),
      object_(object) {}

}  // namespace fem::model
