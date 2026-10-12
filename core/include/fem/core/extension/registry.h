#pragma once

#include "fem/core/extension/contributions.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fem::extension {

/// What the enabled modules contribute (docs/architecture.md §4.2–4.3): the shell builds menus,
/// toolbars, the stage bar, forms and the command palette from it, and rebuilds them when a module
/// is enabled or disabled.
///
/// Records are listed by owner id, then in the order of the manifest, so the order does not depend
/// on the order modules were enabled in. Ids that name something application-wide are unique
/// across modules: commands, stages, solvers, meshers, file formats, activity bar entries, bottom
/// panel tabs, viewport layers, result filters, icons, settings and context keys. Object types
/// (`treeNodeTypes`, `propertySchemas`) belong to their owner and may repeat in other modules.
///
/// The registry is a value: copies share the contributions, so the Extension Manager can try adding
/// several modules to a copy before adding them for real.
class ContributionRegistry {
public:
    /// Adds what a module contributes; `contributions->owner` names it. Throws
    /// ExtensionError(conflict) and adds nothing if the module is already there or an id it
    /// contributes is taken, also within the module itself.
    void add(std::shared_ptr<const Contributions> contributions);
    /// Removes everything a module contributed; returns false if it contributed nothing.
    bool remove_owner(std::string_view owner);
    [[nodiscard]] bool contains(std::string_view owner) const;
    /// Ids of the modules in the registry, sorted.
    [[nodiscard]] std::vector<std::string> owners() const;
    /// What a module contributed, or nullptr.
    [[nodiscard]] const Contributions* find(std::string_view owner) const;

    [[nodiscard]] std::vector<const CommandContribution*> commands() const;
    [[nodiscard]] std::vector<const StageContribution*> stages() const;
    [[nodiscard]] std::vector<const TreeNodeTypeContribution*> tree_node_types() const;
    [[nodiscard]] std::vector<const MenuContribution*> menus() const;
    [[nodiscard]] std::vector<const MenuContribution*> context_menus() const;
    [[nodiscard]] std::vector<const ContextToolbarContribution*> context_toolbar() const;
    [[nodiscard]] std::vector<const PropertySchemaContribution*> property_schemas() const;
    [[nodiscard]] std::vector<const FileFormatContribution*> file_formats() const;
    [[nodiscard]] std::vector<const SolverContribution*> solvers() const;
    [[nodiscard]] std::vector<const MesherContribution*> meshers() const;
    [[nodiscard]] std::vector<const ActivityBarContribution*> activity_bar() const;
    [[nodiscard]] std::vector<const BottomPanelTabContribution*> bottom_panel_tabs() const;
    [[nodiscard]] std::vector<const ViewportLayerContribution*> viewport_layers() const;
    [[nodiscard]] std::vector<const ResultFilterContribution*> result_filters() const;
    [[nodiscard]] std::vector<const SettingContribution*> settings() const;
    [[nodiscard]] std::vector<const KeybindingContribution*> keybindings() const;
    [[nodiscard]] std::vector<const IconContribution*> icons() const;
    [[nodiscard]] std::vector<const LocalizationContribution*> localization() const;
    [[nodiscard]] std::vector<const ContextKeyContribution*> context_keys() const;

    /// The command with this id, or nullptr.
    [[nodiscard]] const CommandContribution* find_command(std::string_view id) const;

private:
    template <class T>
    [[nodiscard]] std::vector<const T*> collect(std::vector<T> Contributions::*member) const;

    std::map<std::string, std::shared_ptr<const Contributions>, std::less<>> owners_;
};

}  // namespace fem::extension
