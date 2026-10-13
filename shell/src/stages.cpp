#include "fem/shell/stages.h"

#include "fem/core/context/value.h"
#include "fem/shell/strings.h"

#include <algorithm>

namespace fem::shell {

bool is_standard_stage(std::string_view id) noexcept {
    return std::find(standard_stages.begin(), standard_stages.end(), id) != standard_stages.end();
}

std::vector<StageEntry> stage_order(const extension::ContributionRegistry& registry,
                                    std::vector<const extension::StageContribution*>* rejected) {
    using Placement = extension::StageContribution::Placement;
    const std::vector<const extension::StageContribution*> stages = registry.stages();
    std::vector<StageEntry> order;
    const auto add_slot = [&](std::string_view anchor, Placement placement) {
        for (const extension::StageContribution* stage : stages) {
            if (stage->anchor == anchor && stage->placement == placement) {
                order.push_back({.id = stage->id, .contribution = stage});
            }
        }
    };
    for (const std::string_view id : standard_stages) {
        add_slot(id, Placement::before);
        order.push_back({.id = std::string(id)});
        add_slot(id, Placement::after);
    }
    if (rejected != nullptr) {
        for (const extension::StageContribution* stage : stages) {
            if (!is_standard_stage(stage->anchor)) {
                rejected->push_back(stage);
            }
        }
    }
    return order;
}

QString stage_title(const l10n::Localization& localization,
                    const extension::ContributionRegistry& registry, std::string_view id) {
    if (is_standard_stage(id)) {
        return text(localization, "stage." + std::string(id));
    }
    for (const extension::StageContribution* stage : registry.stages()) {
        if (stage->id == id) {
            return to_qstring(localization.manifest_text(stage->owner, stage->title));
        }
    }
    return to_qstring(id);
}

void declare_stage_key(context::ContextKeys& keys) {
    (void)keys.declare({.name = std::string(stage_key),
                        .owner = std::string(strings_owner),
                        .type = context::ContextValue::Kind::string,
                        .initial = context::ContextValue(standard_stages.front())});
}

}  // namespace fem::shell
