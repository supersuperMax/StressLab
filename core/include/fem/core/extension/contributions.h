#pragma once

#include "fem/core/command/command.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/value.h"
#include "fem/core/context/when.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Records of the contribution points (docs/architecture.md §4.2): everything a module adds to the
// application, read from its manifest without loading its code. Every record keeps `owner`, the id
// of the module, so the shell can remove what a disabled module added.
//
// Text meant for users (titles, labels, descriptions) is kept as written in the manifest; its
// translation is task 1.7. Paths (icons, catalogs) are relative to the module folder and use '/'.
namespace fem::extension {

/// `commands`: an action with an id, called from menus, the palette, toolbars and Python.
struct CommandContribution {
    std::string owner;
    std::string id;  ///< See `command::is_valid_command_id`.
    std::string title;
    std::string category;  ///< Group in the command palette; may be empty.
    std::string icon;      ///< Path of the icon; empty if none.
    /// Keyword arguments: the Python signature of the command (docs/architecture.md §4.7).
    std::vector<command::ArgSpec> args;
};

/// `stages`: a stage of the stage bar, in a slot before or after a standard stage.
struct StageContribution {
    enum class Placement : std::uint8_t { before, after };

    std::string owner;
    std::string id;
    std::string title;
    std::string icon;
    Placement placement = Placement::after;
    std::string anchor;  ///< The stage this one goes before or after.
    std::optional<context::WhenExpression> when;
};

/// `treeNodeTypes`: how objects of a type of the module show in the model tree.
struct TreeNodeTypeContribution {
    std::string owner;
    std::string type;  ///< Object type in the Model IR, owned by the module.
    std::string title;
    std::string icon;
};

/// `menus` and `contextMenus`: where a command shows, and when.
struct MenuContribution {
    std::string owner;
    std::string location;  ///< Menu path, e.g. "solve/run", or context, e.g. "tree".
    std::string command;   ///< Any command, also of another module.
    std::optional<context::WhenExpression> when;
    std::string group;  ///< Group and order within the menu; may be empty.
};

/// `contextToolbar`: a command on the context toolbar, and when it shows.
struct ContextToolbarContribution {
    std::string owner;
    std::string command;
    std::optional<context::WhenExpression> when;
    std::string group;
};

/// `propertySchemas`: the property form of an object type of the module (JSON Schema).
struct PropertySchemaContribution {
    std::string owner;
    std::string type;           ///< Object type in the Model IR.
    std::uint32_t version = 1;  ///< Schema version of the objects it describes.
    nlohmann::json schema = nlohmann::json::object();
};

/// `fileFormats`: opening, import and export of models, meshes and results.
struct FileFormatContribution {
    enum class Role : std::uint8_t {
        model_import,
        model_export,
        mesh_import,
        mesh_export,
        results_import,
        results_export,
    };

    std::string owner;
    std::string id;
    std::string title;                    ///< May be empty.
    std::vector<std::string> extensions;  ///< With the dot, e.g. ".res".
    Role role = Role::model_import;
};

/// "model-import", "model-export", "mesh-import", "mesh-export", "results-import",
/// "results-export".
[[nodiscard]] std::string_view to_string(FileFormatContribution::Role role) noexcept;

/// `solvers`: a solver adapter (docs/architecture.md §6).
struct SolverContribution {
    std::string owner;
    std::string id;  ///< One segment: it is a part of context keys `solver.<id>.available`.
    std::string label;
    std::vector<std::string> analyses;  ///< E.g. "static"; may be empty.
};

/// `meshers`: a mesh generator.
struct MesherContribution {
    std::string owner;
    std::string id;  ///< One segment, like a solver id.
    std::string label;
    /// Object type of the parameters, with a `propertySchemas` entry of the module; may be empty.
    std::string parameters;
};

/// `activityBar`: an icon with a side panel; at most one per module.
struct ActivityBarContribution {
    std::string owner;
    std::string id;
    std::string title;
    std::string icon;
};

/// `bottomPanelTabs`: a tab of the bottom panel.
struct BottomPanelTabContribution {
    std::string owner;
    std::string id;
    std::string title;
    std::optional<context::WhenExpression> when;
};

/// `viewportLayers`: a display layer or post-processing tool of the 3D view.
struct ViewportLayerContribution {
    std::string owner;
    std::string id;
    std::string title;
    std::optional<context::WhenExpression> when;
};

/// `resultFilters`: a post-processing operation on results.
struct ResultFilterContribution {
    std::string owner;
    std::string id;
    std::string title;
};

/// `settings`: a setting of the module. Values are checked against the schema in task 1.7.
struct SettingContribution {
    enum class Type : std::uint8_t { boolean, integer, number, string, path };

    std::string owner;
    std::string key;  ///< E.g. "solver.threads".
    Type type = Type::string;
    nlohmann::json default_value;  ///< Null if none.
    std::string description;
    /// The whole entry of the manifest as JSON (type, default, description, enum, minimum,
    /// maximum), for the settings form and validation.
    nlohmann::json schema = nlohmann::json::object();
};

/// "boolean", "integer", "number", "string", "path".
[[nodiscard]] std::string_view to_string(SettingContribution::Type type) noexcept;

/// `keybindings`: a shortcut for a command.
struct KeybindingContribution {
    std::string owner;
    std::string command;
    /// Modifiers and a key joined by '+', e.g. "Mod+Shift+R". `Mod` is Ctrl, or Cmd on macOS.
    std::string key;
    std::optional<context::WhenExpression> when;
};

/// `icons`: an icon other contributions can refer to by id.
struct IconContribution {
    std::string owner;
    std::string id;
    std::string path;
};

/// `localization`: a catalog of translated strings (format: task 1.7).
struct LocalizationContribution {
    std::string owner;
    std::string language;  ///< E.g. "ru", "en", "pt-BR".
    std::string path;
};

/// `contextKeys`: a context key of the module, declared when it is enabled, so `when` clauses can
/// read it before the code of the module is loaded. Not in the table of docs/architecture.md §4.2;
/// see docs/adr/0006-extension-manager.md.
struct ContextKeyContribution {
    std::string owner;
    std::string name;
    context::ContextValue::Kind type = context::ContextValue::Kind::boolean;
    context::ContextValue initial;  ///< None if the manifest gives no default.

    [[nodiscard]] context::KeySpec spec() const {
        return {.name = name, .owner = owner, .type = type, .initial = initial};
    }
};

/// Everything one module contributes, in the order of its manifest.
struct Contributions {
    std::string owner;
    std::vector<CommandContribution> commands;
    std::vector<StageContribution> stages;
    std::vector<TreeNodeTypeContribution> tree_node_types;
    std::vector<MenuContribution> menus;
    std::vector<MenuContribution> context_menus;
    std::vector<ContextToolbarContribution> context_toolbar;
    std::vector<PropertySchemaContribution> property_schemas;
    std::vector<FileFormatContribution> file_formats;
    std::vector<SolverContribution> solvers;
    std::vector<MesherContribution> meshers;
    std::vector<ActivityBarContribution> activity_bar;
    std::vector<BottomPanelTabContribution> bottom_panel_tabs;
    std::vector<ViewportLayerContribution> viewport_layers;
    std::vector<ResultFilterContribution> result_filters;
    std::vector<SettingContribution> settings;
    std::vector<KeybindingContribution> keybindings;
    std::vector<IconContribution> icons;
    std::vector<LocalizationContribution> localization;
    std::vector<ContextKeyContribution> context_keys;
};

}  // namespace fem::extension
