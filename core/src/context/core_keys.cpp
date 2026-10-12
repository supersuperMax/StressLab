#include "fem/core/context/core_keys.h"

#include "fem/core/model/document.h"
#include "fem/core/model/object.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace fem::context {

namespace {

bool is_mesh_of_the_core(const model::Object& object) {
    return object.type() == model::core_types::mesh && object.owner() == model::core_owner;
}

}  // namespace

CoreKeys::CoreKeys(command::CommandBus& bus, ContextKeys& keys) : bus_(bus), keys_(keys) {
    for (const std::string_view name :
         {core_keys::has_mesh, core_keys::can_undo, core_keys::can_redo}) {
        keys_.declare({.name = std::string(name),
                       .owner = std::string(model::core_owner),
                       .type = ContextValue::Kind::boolean,
                       .initial = false});
    }
    const model::Document& document = bus_.document();
    for (const model::ObjectId id : document.find_by_type(model::core_types::mesh)) {
        if (is_mesh_of_the_core(document.get(id))) {
            meshes_.insert(id);
        }
    }
    update();
    model_changed_ = bus_.on_model_changed(
        [this](const command::ModelChange& change) { on_model_changed(change); });
}

void CoreKeys::on_model_changed(const command::ModelChange& change) {
    // An object keeps its type, so only created and removed objects matter.
    for (const model::ObjectId id : change.summary.removed) {
        meshes_.erase(id);
    }
    const model::Document& document = bus_.document();
    for (const model::ObjectId id : change.summary.created) {
        if (is_mesh_of_the_core(document.get(id))) {
            meshes_.insert(id);
        }
    }
    update();
}

void CoreKeys::update() {
    keys_.set({{std::string(core_keys::has_mesh), !meshes_.empty()},
               {std::string(core_keys::can_undo), bus_.can_undo()},
               {std::string(core_keys::can_redo), bus_.can_redo()}});
}

}  // namespace fem::context
