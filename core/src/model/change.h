#pragma once

// Records of a ChangeSet. Each record holds the value its change replaced; applying it swaps that
// value with the one in the document, so the same record serves undo and redo.

#include "fem/core/model/change_set.h"
#include "fem/core/model/object.h"
#include "fem/core/model/object_data.h"
#include "fem/core/model/object_id.h"
#include "fem/core/model/units.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <variant>

namespace fem::model {

namespace change {

/// An object entered or left the document. While it is out, the record holds its map node
/// (the object itself, never copied) and its index among its siblings.
struct Existence {
    ObjectId id;
    std::map<ObjectId, Object>::node_type node;
    std::size_t position = 0;
};

struct Name {
    ObjectId id;
    std::string name;
};

struct PropertyValues {
    ObjectId id;
    Properties properties;
};

struct Data {
    ObjectId id;
    std::shared_ptr<const ObjectData> data;
};

/// `id` started or stopped depending on `target`.
struct Dependency {
    ObjectId id;
    ObjectId target;
};

struct Units {
    UnitSystem units;
};

}  // namespace change

struct ChangeSet::Change {
    std::variant<change::Existence, change::Name, change::PropertyValues, change::Data,
                 change::Dependency, change::Units>
        record;
};

}  // namespace fem::model
