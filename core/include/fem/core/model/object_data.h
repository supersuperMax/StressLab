#pragma once

namespace fem::model {

/// Bulk data attached to a model object, such as a mesh or a node set.
///
/// Small, editable values of an object live in its JSON properties; large arrays live in an
/// `ObjectData`. Object data is immutable once attached to a document and is shared through
/// `std::shared_ptr<const T>`: readers (Python views, the viewer, the blob store) keep it alive
/// without copying, and replacing it is a cheap pointer swap that undo can revert.
class ObjectData {
public:
    virtual ~ObjectData() = default;

protected:
    ObjectData() = default;
    ObjectData(const ObjectData&) = default;
    ObjectData(ObjectData&&) = default;
    ObjectData& operator=(const ObjectData&) = default;
    ObjectData& operator=(ObjectData&&) = default;
};

}  // namespace fem::model
