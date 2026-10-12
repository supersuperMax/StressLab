#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/model/object_id.h"
#include "fem/core/signal.h"

#include <set>
#include <string_view>

namespace fem::context {

/// Context keys of the core, owned by "fem.core". Keys of the shell (`stage`, `selection.*`,
/// `tree.*`), of jobs (`job.*`) and of solvers (`solver.<id>.*`) are declared by their owners.
namespace core_keys {
/// The document has a mesh (an object of the core type "mesh").
inline constexpr std::string_view has_mesh = "model.hasMesh";
/// There is a step to undo (`CommandBus::can_undo`).
inline constexpr std::string_view can_undo = "history.canUndo";
/// There is a step to redo (`CommandBus::can_redo`).
inline constexpr std::string_view can_redo = "history.canRedo";
}  // namespace core_keys

/// Keeps the keys of the core in step with a document and its Command Bus. The keys change when
/// the model changes, that is after a command, a committed transaction, undo or redo, never in
/// the middle of a transaction.
///
/// Keep one for the document being edited: two of them would set the same keys. The keys are
/// declared once per `ContextKeys` and keep their values when this object is destroyed, until the
/// next one sets them.
class CoreKeys {
public:
    /// Declares the keys in `keys` (unless they are declared) and sets them from `bus`. Both must
    /// outlive this object.
    CoreKeys(command::CommandBus& bus, ContextKeys& keys);
    CoreKeys(const CoreKeys&) = delete;
    CoreKeys& operator=(const CoreKeys&) = delete;
    CoreKeys(CoreKeys&&) = delete;
    CoreKeys& operator=(CoreKeys&&) = delete;
    ~CoreKeys() = default;

private:
    void on_model_changed(const command::ModelChange& change);
    void update();

    command::CommandBus& bus_;
    ContextKeys& keys_;
    std::set<model::ObjectId> meshes_;
    Subscription model_changed_;
};

}  // namespace fem::context
