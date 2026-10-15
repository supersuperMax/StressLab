#include "fem/core/model/document_editor.h"

#include "fem/core/model/error.h"

#include <utility>

namespace fem::model {

ObjectId DocumentEditor::create(NewObject init) {
    return document_.create(std::move(init), changes());
}

void DocumentEditor::rename(ObjectId id, std::string name) {
    document_.rename(id, std::move(name), changes());
}

void DocumentEditor::set_properties(ObjectId id, Properties properties) {
    document_.set_properties(id, std::move(properties), changes());
}

void DocumentEditor::set_data(ObjectId id, std::shared_ptr<const ObjectData> data) {
    document_.set_data(id, std::move(data), changes());
}

bool DocumentEditor::add_dependency(ObjectId dependent, ObjectId target) {
    return document_.add_dependency(dependent, target, changes());
}

bool DocumentEditor::remove_dependency(ObjectId dependent, ObjectId target) {
    return document_.remove_dependency(dependent, target, changes());
}

RemoveResult DocumentEditor::remove(ObjectId id) {
    return document_.remove(id, changes());
}

void DocumentEditor::set_units(const UnitSystem& units) {
    document_.set_units(units, changes());
}

ChangeSet DocumentEditor::new_changes() const noexcept {
    return document_.new_changes();
}

ChangeSet* DocumentEditor::record_into(ChangeSet* changes) noexcept {
    return std::exchange(changes_, changes);
}

void DocumentEditor::rollback(ChangeSet& changes) {
    document_.rollback(changes);
}

void DocumentEditor::undo(ChangeSet& changes) {
    document_.check_history(changes);
    document_.apply(changes, true);
}

void DocumentEditor::redo(ChangeSet& changes) {
    document_.check_history(changes);
    document_.apply(changes, false);
}

void DocumentEditor::replace(DocumentContents contents) {
    document_.replace(std::move(contents));
}

ChangeSet& DocumentEditor::changes() {
    if (changes_ == nullptr) {
        throw ModelError(ModelErrc::outside_command,
                         "the model can be changed only by a command run by the Command Bus");
    }
    return *changes_;
}

}  // namespace fem::model
