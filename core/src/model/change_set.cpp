#include "fem/core/model/change_set.h"

#include "change.h"

#include <iterator>
#include <map>
#include <type_traits>
#include <variant>

namespace fem::model {

ChangeSet::ChangeSet(std::uint64_t next_id) noexcept : next_id_(next_id) {}
ChangeSet::ChangeSet(ChangeSet&& other) noexcept = default;
ChangeSet& ChangeSet::operator=(ChangeSet&& other) noexcept = default;
ChangeSet::~ChangeSet() = default;

bool ChangeSet::empty() const noexcept {
    return changes_.empty();
}

std::size_t ChangeSet::size() const noexcept {
    return changes_.size();
}

void ChangeSet::append(ChangeSet&& later) {
    changes_.insert(changes_.end(), std::make_move_iterator(later.changes_.begin()),
                    std::make_move_iterator(later.changes_.end()));
    later.changes_.clear();
}

ChangeSummary ChangeSet::summary() const {
    struct Presence {
        bool before;
        bool after;
    };
    std::map<ObjectId, Presence> objects;
    ChangeSummary summary;

    const auto visit = [&](const Change& change) {
        std::visit(
            [&](const auto& record) {
                using Record = std::decay_t<decltype(record)>;
                if constexpr (std::is_same_v<Record, change::Units>) {
                    summary.units = true;
                } else if constexpr (std::is_same_v<Record, change::Existence>) {
                    // A record that holds the object took it out when it was last applied.
                    const bool present = record.node.empty();
                    const auto [entry, inserted] =
                        objects.try_emplace(record.id, Presence{!present, present});
                    if (!inserted) {
                        entry->second.after = present;
                    }
                } else {
                    objects.try_emplace(record.id, Presence{true, true});
                }
            },
            change.record);
    };
    // The order in which the records were last applied.
    if (undone_) {
        for (auto change = changes_.rbegin(); change != changes_.rend(); ++change) {
            visit(*change);
        }
    } else {
        for (const Change& change : changes_) {
            visit(change);
        }
    }

    for (const auto& [id, presence] : objects) {
        if (!presence.before && presence.after) {
            summary.created.push_back(id);
        } else if (presence.before && !presence.after) {
            summary.removed.push_back(id);
        } else if (presence.before && presence.after) {
            summary.updated.push_back(id);
        }
    }
    return summary;
}

}  // namespace fem::model
