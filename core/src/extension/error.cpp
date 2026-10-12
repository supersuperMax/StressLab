#include "fem/core/extension/error.h"

#include <algorithm>
#include <utility>

namespace fem::extension {

namespace {

/// Sorts the diagnostics by position and describes them, one line each.
std::string describe(const std::filesystem::path& file,
                     std::vector<ManifestDiagnostic>& diagnostics) {
    std::stable_sort(diagnostics.begin(), diagnostics.end(),
                     [](const ManifestDiagnostic& a, const ManifestDiagnostic& b) {
                         return a.line != b.line ? a.line < b.line : a.column < b.column;
                     });
    const std::u8string name = file.empty() ? u8"manifest" : file.generic_u8string();
    const std::string prefix(name.begin(), name.end());
    std::string text;
    for (const ManifestDiagnostic& diagnostic : diagnostics) {
        if (!text.empty()) {
            text += '\n';
        }
        text += prefix;
        if (diagnostic.line > 0) {
            text += ':' + std::to_string(diagnostic.line) + ':' + std::to_string(diagnostic.column);
        }
        text += ": " + diagnostic.message;
    }
    return text;
}

}  // namespace

std::string_view to_string(ExtensionErrc code) noexcept {
    switch (code) {
        case ExtensionErrc::invalid_manifest:
            return "invalid_manifest";
        case ExtensionErrc::invalid_version:
            return "invalid_version";
        case ExtensionErrc::unknown_extension:
            return "unknown_extension";
        case ExtensionErrc::incompatible:
            return "incompatible";
        case ExtensionErrc::unresolved_dependency:
            return "unresolved_dependency";
        case ExtensionErrc::dependency_cycle:
            return "dependency_cycle";
        case ExtensionErrc::conflict:
            return "conflict";
        case ExtensionErrc::activation_failed:
            return "activation_failed";
        case ExtensionErrc::command_failed:
            return "command_failed";
        case ExtensionErrc::invalid_state:
            return "invalid_state";
        case ExtensionErrc::invalid_profile:
            return "invalid_profile";
        case ExtensionErrc::io:
            return "io";
    }
    return "unknown";
}

ExtensionError::ExtensionError(ExtensionErrc code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

// The base is initialized first: `describe` sorts `diagnostics` before it is moved into the member.
ManifestError::ManifestError(std::filesystem::path file,
                             std::vector<ManifestDiagnostic> diagnostics)
    : ExtensionError(ExtensionErrc::invalid_manifest, describe(file, diagnostics)),
      file_(std::move(file)),
      diagnostics_(std::move(diagnostics)) {}

}  // namespace fem::extension
