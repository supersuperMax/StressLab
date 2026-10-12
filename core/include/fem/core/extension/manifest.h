#pragma once

#include "fem/core/extension/contributions.h"
#include "fem/core/extension/semver.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fem::extension {

/// Name of the manifest file in the folder of a module.
inline constexpr std::string_view manifest_file_name = "stresslab-extension.toml";

/// How the code of a module runs (docs/architecture.md §4.4).
enum class ExtensionKind : std::uint8_t {
    native,   ///< A shared library in the application process, through the C ABI.
    python,   ///< A Python package in the extension host process (task 1.9).
    process,  ///< An executable of its own (task 1.9).
};

/// "native", "python", "process".
[[nodiscard]] std::string_view to_string(ExtensionKind kind) noexcept;

/// True if `id` can be the id of a module: lowercase segments of ASCII letters, digits, '_' and
/// '-', starting with a letter, two or more joined by '.' ("fem.mesher.gmsh"). "fem.core" is the
/// platform itself, not a module.
[[nodiscard]] bool is_valid_extension_id(std::string_view id) noexcept;

/// A module this one needs, with the versions it accepts.
struct Requirement {
    std::string id;
    VersionRange range;

    friend bool operator==(const Requirement&, const Requirement&) = default;
};

/// When the code of a module is loaded. In a manifest, `value` is a pattern where '*' matches any
/// text and '?' one character; a fired event carries the command id, file name or solver id.
struct ActivationEvent {
    enum class Trigger : std::uint8_t {
        startup,           ///< "onStartup": when the application has started.
        command,           ///< "onCommand:<command id>"
        open_file,         ///< "onOpenFile:<file name>": compared without ASCII case.
        solver_requested,  ///< "onSolverRequested:<solver id>"
    };

    Trigger trigger = Trigger::startup;
    std::string value;  ///< Empty for startup.

    [[nodiscard]] static ActivationEvent startup() { return {}; }
    [[nodiscard]] static ActivationEvent command(std::string id) {
        return {Trigger::command, std::move(id)};
    }
    /// The event for a file: its name, without the folders.
    [[nodiscard]] static ActivationEvent open_file(const std::filesystem::path& file);
    [[nodiscard]] static ActivationEvent solver_requested(std::string id) {
        return {Trigger::solver_requested, std::move(id)};
    }

    /// Reads the form of a manifest; nothing if the text is not an activation event.
    [[nodiscard]] static std::optional<ActivationEvent> parse(std::string_view text);
    /// The form of a manifest, e.g. "onCommand:solver.*".
    [[nodiscard]] std::string to_string() const;
    /// True if this event, written in a manifest, waits for the fired event `fired`.
    [[nodiscard]] bool matches(const ActivationEvent& fired) const;

    friend bool operator==(const ActivationEvent&, const ActivationEvent&) = default;
};

/// What `stresslab-extension.toml` says about a module (docs/architecture.md §4.1):
///
///     [extension]
///     id = "fem.example"            # required
///     name = "Example"              # required, shown to users
///     version = "1.2.0"             # required, Semantic Versioning
///     engine = "^1.0"               # required, versions of the platform the module runs on
///     description = "..."
///     kind = "python"               # native | python | process; with `entry`, or neither
///     entry = "example:activate"    # native: library path without extension;
///                                   # python: package.module:function; process: executable path
///     requires = ["fem.other>=1.0"] # modules it needs, with optional version ranges
///     activation = ["onCommand:example.*"]
///
///     [[contributes.commands]]      # contribution points, see contributions.h
///     ...
///
/// A module without `kind` and `entry` has no code: it contributes only what needs none (menus for
/// commands of other modules, keybindings, settings, translations).
struct Manifest {
    std::filesystem::path file;  ///< Where it was read from; empty if parsed from text.
    std::string id;
    std::string name;
    std::string description;
    Version version;
    VersionRange engine;
    std::optional<ExtensionKind> kind;  ///< None for a module without code.
    std::string entry;
    std::vector<Requirement> requirements;
    std::vector<ActivationEvent> activation;
    /// Empty if `engine` does not accept the platform version the manifest was parsed for.
    Contributions contributes;

    [[nodiscard]] bool has_code() const noexcept { return kind.has_value(); }
};

/// Parses a manifest for a platform of version `engine`. Unknown keys, values of a wrong type and
/// missing required keys are errors; all of them are reported at once by ManifestError, with lines
/// and columns. If the `engine` range of the module does not accept `engine`, only the id, name,
/// version and engine are checked and `contributes` stays empty: a module for another version of
/// the platform may use keys this one does not know. `file` is only used in messages.
[[nodiscard]] Manifest parse_manifest(std::string_view text, const Version& engine,
                                      const std::filesystem::path& file = {});

/// Reads and parses a manifest file. Throws ExtensionError(io) if it cannot be read and
/// ManifestError if it is not valid.
[[nodiscard]] Manifest load_manifest(const std::filesystem::path& file, const Version& engine);

}  // namespace fem::extension
