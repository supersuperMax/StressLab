#pragma once

#include "fem/core/context/context_keys.h"
#include "fem/core/extension/contributions.h"
#include "fem/core/extension/registry.h"
#include "fem/core/l10n/localization.h"

#include <QString>
#include <array>
#include <string>
#include <string_view>
#include <vector>

// The stage bar (docs/interface-design.md §4–5): the standard stages of the workflow, in a fixed
// order, and the stages of modules in the slots before and after them (docs/architecture.md §4.2,
// item 9 of §4.6). Modules cannot move or remove the standard stages.
namespace fem::shell {

/// The context key of the current stage (ADR 0005: owned by the shell): the id of a stage.
inline constexpr std::string_view stage_key = "stage";

/// Ids of the standard stages, in their order. Their titles are the strings "stage.<id>" of the
/// shell.
inline constexpr std::array<std::string_view, 6> standard_stages{"geometry", "mesh",  "materials",
                                                                 "steps",    "solve", "results"};

/// True if `id` is a standard stage.
[[nodiscard]] bool is_standard_stage(std::string_view id) noexcept;

/// A stage of the stage bar.
struct StageEntry {
    std::string id;
    /// The stage of a module; nullptr for a standard stage.
    const extension::StageContribution* contribution = nullptr;
};

/// The stages in their order: the standard ones, each with the stages of modules before and after
/// it. Several stages in one slot keep the order of the registry (by module id, then as in the
/// manifest). A stage whose anchor is not a standard stage is left out and added to `rejected`.
[[nodiscard]] std::vector<StageEntry> stage_order(
    const extension::ContributionRegistry& registry,
    std::vector<const extension::StageContribution*>* rejected = nullptr);

/// The title of a stage in the current language: of a standard stage from the strings of the
/// shell, of a stage of a module from its manifest; the id if there is no such stage.
[[nodiscard]] QString stage_title(const l10n::Localization& localization,
                                  const extension::ContributionRegistry& registry,
                                  std::string_view id);

/// Declares `stage` (owner "fem.shell", text) in `keys` unless it is declared, with the first
/// standard stage as its value.
void declare_stage_key(context::ContextKeys& keys);

}  // namespace fem::shell
