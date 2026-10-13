#include "fem/core/extension/registry.h"

#include "fem/core/extension/error.h"

#include <set>
#include <utility>

namespace fem::extension {

namespace {

/// An id that names something application-wide, with what it names.
using GlobalId = std::pair<std::string_view, std::string_view>;

std::vector<GlobalId> global_ids(const Contributions& c) {
    std::vector<GlobalId> ids;
    const auto add = [&ids](std::string_view what, const auto& records, auto member) {
        for (const auto& record : records) {
            ids.emplace_back(what, record.*member);
        }
    };
    add("command", c.commands, &CommandContribution::id);
    add("stage", c.stages, &StageContribution::id);
    add("file format", c.file_formats, &FileFormatContribution::id);
    add("solver", c.solvers, &SolverContribution::id);
    add("mesher", c.meshers, &MesherContribution::id);
    add("activity bar entry", c.activity_bar, &ActivityBarContribution::id);
    add("bottom panel tab", c.bottom_panel_tabs, &BottomPanelTabContribution::id);
    add("submenu", c.submenus, &SubmenuContribution::id);
    add("viewport layer", c.viewport_layers, &ViewportLayerContribution::id);
    add("result filter", c.result_filters, &ResultFilterContribution::id);
    add("icon", c.icons, &IconContribution::id);
    add("setting", c.settings, &SettingContribution::key);
    add("context key", c.context_keys, &ContextKeyContribution::name);
    return ids;
}

[[noreturn]] void conflict(const std::string& message) {
    throw ExtensionError(ExtensionErrc::conflict, message);
}

}  // namespace

void ContributionRegistry::add(std::shared_ptr<const Contributions> contributions) {
    if (!contributions || contributions->owner.empty()) {
        throw ExtensionError(ExtensionErrc::invalid_state, "contributions without an owner");
    }
    const std::string& owner = contributions->owner;
    if (owners_.contains(owner)) {
        conflict("module '" + owner + "' is already in the registry");
    }
    const std::vector<GlobalId> ids = global_ids(*contributions);
    std::set<GlobalId> own;
    for (const GlobalId& id : ids) {
        if (!own.insert(id).second) {
            conflict("module '" + owner + "' contributes " + std::string(id.first) + " '" +
                     std::string(id.second) + "' twice");
        }
    }
    for (const auto& [other, records] : owners_) {
        for (const GlobalId& id : global_ids(*records)) {
            if (own.contains(id)) {
                conflict(std::string(id.first) + " '" + std::string(id.second) + "' of module '" +
                         owner + "' is already contributed by module '" + other + "'");
            }
        }
    }
    owners_.emplace(owner, std::move(contributions));
}

bool ContributionRegistry::remove_owner(std::string_view owner) {
    const auto it = owners_.find(owner);
    if (it == owners_.end()) {
        return false;
    }
    owners_.erase(it);
    return true;
}

bool ContributionRegistry::contains(std::string_view owner) const {
    return owners_.contains(owner);
}

std::vector<std::string> ContributionRegistry::owners() const {
    std::vector<std::string> ids;
    ids.reserve(owners_.size());
    for (const auto& [owner, records] : owners_) {
        ids.push_back(owner);
    }
    return ids;
}

const Contributions* ContributionRegistry::find(std::string_view owner) const {
    const auto it = owners_.find(owner);
    return it == owners_.end() ? nullptr : it->second.get();
}

template <class T>
std::vector<const T*> ContributionRegistry::collect(std::vector<T> Contributions::*member) const {
    std::vector<const T*> result;
    for (const auto& [owner, records] : owners_) {
        for (const T& record : (*records).*member) {
            result.push_back(&record);
        }
    }
    return result;
}

std::vector<const CommandContribution*> ContributionRegistry::commands() const {
    return collect(&Contributions::commands);
}

std::vector<const StageContribution*> ContributionRegistry::stages() const {
    return collect(&Contributions::stages);
}

std::vector<const TreeNodeTypeContribution*> ContributionRegistry::tree_node_types() const {
    return collect(&Contributions::tree_node_types);
}

std::vector<const MenuContribution*> ContributionRegistry::menus() const {
    return collect(&Contributions::menus);
}

std::vector<const MenuContribution*> ContributionRegistry::context_menus() const {
    return collect(&Contributions::context_menus);
}

std::vector<const SubmenuContribution*> ContributionRegistry::submenus() const {
    return collect(&Contributions::submenus);
}

std::vector<const ContextToolbarContribution*> ContributionRegistry::context_toolbar() const {
    return collect(&Contributions::context_toolbar);
}

std::vector<const PropertySchemaContribution*> ContributionRegistry::property_schemas() const {
    return collect(&Contributions::property_schemas);
}

std::vector<const FileFormatContribution*> ContributionRegistry::file_formats() const {
    return collect(&Contributions::file_formats);
}

std::vector<const SolverContribution*> ContributionRegistry::solvers() const {
    return collect(&Contributions::solvers);
}

std::vector<const MesherContribution*> ContributionRegistry::meshers() const {
    return collect(&Contributions::meshers);
}

std::vector<const ActivityBarContribution*> ContributionRegistry::activity_bar() const {
    return collect(&Contributions::activity_bar);
}

std::vector<const BottomPanelTabContribution*> ContributionRegistry::bottom_panel_tabs() const {
    return collect(&Contributions::bottom_panel_tabs);
}

std::vector<const ViewportLayerContribution*> ContributionRegistry::viewport_layers() const {
    return collect(&Contributions::viewport_layers);
}

std::vector<const ResultFilterContribution*> ContributionRegistry::result_filters() const {
    return collect(&Contributions::result_filters);
}

std::vector<const SettingContribution*> ContributionRegistry::settings() const {
    return collect(&Contributions::settings);
}

std::vector<const KeybindingContribution*> ContributionRegistry::keybindings() const {
    return collect(&Contributions::keybindings);
}

std::vector<const IconContribution*> ContributionRegistry::icons() const {
    return collect(&Contributions::icons);
}

std::vector<const LocalizationContribution*> ContributionRegistry::localization() const {
    return collect(&Contributions::localization);
}

std::vector<const ContextKeyContribution*> ContributionRegistry::context_keys() const {
    return collect(&Contributions::context_keys);
}

const CommandContribution* ContributionRegistry::find_command(std::string_view id) const {
    for (const auto& [owner, records] : owners_) {
        for (const CommandContribution& command : records->commands) {
            if (command.id == id) {
                return &command;
            }
        }
    }
    return nullptr;
}

}  // namespace fem::extension
