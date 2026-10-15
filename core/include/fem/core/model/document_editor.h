#pragma once

#include "fem/core/model/change_set.h"
#include "fem/core/model/contents.h"
#include "fem/core/model/document.h"
#include "fem/core/model/object.h"
#include "fem/core/model/object_data.h"
#include "fem/core/model/object_id.h"
#include "fem/core/model/units.h"

#include <memory>
#include <string>

namespace fem::command {
class CommandBus;
}

namespace fem::model {

/// The only way to change a `Document` (CLAUDE.md, rule 3).
///
/// The Command Bus owns the editor of its document and hands it to commands while they run. Every
/// change is recorded, so the bus can undo and redo it and roll back a command that failed;
/// commands do not write undo code. Outside a running command every change throws
/// ModelError(outside_command), so an editor kept by a command after it returned is useless.
///
/// The changes validate their arguments and throw ModelError, leaving the document unchanged,
/// if the request is invalid. A change that sets the current value records nothing.
class DocumentEditor {
public:
    DocumentEditor(const DocumentEditor&) = delete;
    DocumentEditor& operator=(const DocumentEditor&) = delete;
    DocumentEditor(DocumentEditor&&) = delete;
    DocumentEditor& operator=(DocumentEditor&&) = delete;
    ~DocumentEditor() = default;

    [[nodiscard]] const Document& document() const noexcept { return document_; }

    /// Creates an object and returns its new id. The object may belong to a disabled module
    /// (an importer may meet objects of a module that is not installed), but its parent must not
    /// be opaque.
    ObjectId create(NewObject init);
    void rename(ObjectId id, std::string name);
    void set_properties(ObjectId id, Properties properties);
    /// Replaces the bulk data. Replacing a mesh checks the sets under it against the new mesh.
    void set_data(ObjectId id, std::shared_ptr<const ObjectData> data);

    /// Returns false if the dependency already exists. Refuses cycles and missing targets.
    bool add_dependency(ObjectId dependent, ObjectId target);
    /// Returns false if there was no such dependency.
    bool remove_dependency(ObjectId dependent, ObjectId target);

    /// Removes `id` with its subtree, unless that would lose or break an opaque object; then
    /// nothing changes and the result lists the reasons. Active objects that depend on a removed
    /// object keep the dependency, which becomes missing (`Document::missing_dependencies`).
    [[nodiscard]] RemoveResult remove(ObjectId id);

    /// Declares the unit system of the model. Values are not converted.
    void set_units(const UnitSystem& units);

private:
    friend class fem::command::CommandBus;

    explicit DocumentEditor(Document& document) noexcept : document_(document) {}

    /// An empty change set that starts at the current state of the document.
    [[nodiscard]] ChangeSet new_changes() const noexcept;
    /// Where changes are recorded from now on (nullptr: nowhere, changes throw). Returns the
    /// previous target.
    ChangeSet* record_into(ChangeSet* changes) noexcept;
    [[nodiscard]] ChangeSet* recording() const noexcept { return changes_; }

    /// Reverts changes of a command or transaction that did not complete, including the ids they
    /// took, and empties the set. Never refused.
    void rollback(ChangeSet& changes);
    /// Applies a completed step backwards or forwards. Throws ModelError(opaque_object) and
    /// changes nothing if that would change an object of a disabled module.
    void undo(ChangeSet& changes);
    void redo(ChangeSet& changes);
    /// Makes the document hold other contents (`Document::replace`). Not recorded: the bus drops
    /// its steps, which belong to the old contents.
    void replace(DocumentContents contents);

    ChangeSet& changes();

    Document& document_;
    ChangeSet* changes_ = nullptr;
};

}  // namespace fem::model
