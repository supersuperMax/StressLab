#pragma once

#include "fem/core/model/object.h"
#include "fem/core/model/object_id.h"
#include "fem/core/model/units.h"

#include <cstdint>
#include <vector>

namespace fem::model {

/// An object as it was saved: its id and everything `NewObject` holds. Children are not listed:
/// they name their parent, and siblings are in id order, which is creation order.
struct StoredObject {
    ObjectId id{};
    NewObject init{};
};

/// The whole model as a project stores it (task 2.1): what a document becomes when a project is
/// opened. Loading is not a change of the model, so it goes past the commands and their undo
/// (`CommandBus::replace_document`) but is checked as strictly as they are.
struct DocumentContents {
    UnitSystem units{};
    /// The id the next created object gets: greater than every id the document has issued, also
    /// those of removed objects, so ids are never reused.
    std::uint64_t next_id = 1;
    /// In any order.
    std::vector<StoredObject> objects{};
};

}  // namespace fem::model
