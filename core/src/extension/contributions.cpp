#include "fem/core/extension/contributions.h"

namespace fem::extension {

std::string_view to_string(FileFormatContribution::Role role) noexcept {
    using Role = FileFormatContribution::Role;
    switch (role) {
        case Role::model_import:
            return "model-import";
        case Role::model_export:
            return "model-export";
        case Role::mesh_import:
            return "mesh-import";
        case Role::mesh_export:
            return "mesh-export";
        case Role::results_import:
            return "results-import";
        case Role::results_export:
            return "results-export";
    }
    return "unknown";
}

std::string_view to_string(SettingContribution::Type type) noexcept {
    using Type = SettingContribution::Type;
    switch (type) {
        case Type::boolean:
            return "boolean";
        case Type::integer:
            return "integer";
        case Type::number:
            return "number";
        case Type::string:
            return "string";
        case Type::path:
            return "path";
    }
    return "unknown";
}

}  // namespace fem::extension
