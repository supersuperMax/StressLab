#include "fem/core/command/command.h"
#include "fem/core/command/command_bus.h"
#include "fem/core/command/journal.h"
#include "fem/core/model/document.h"
#include "fem/core/model/units.h"

#include "common.h"
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fem::python {

namespace {

using command::ArgSpec;
using command::CommandSpec;
using model::ObjectId;

// --- Helpers -------------------------------------------------------------------------------------

nb::object object_or_none(const std::shared_ptr<DocumentHandle>& handle, ObjectId id) {
    if (!id.is_valid()) {
        return nb::none();
    }
    return nb::cast(PyModelObject{handle, id});
}

nb::list objects_of(const std::shared_ptr<DocumentHandle>& handle, auto&& ids) {
    nb::list list;
    for (const ObjectId id : ids) {
        list.append(PyModelObject{handle, id});
    }
    return list;
}

nb::list ids_of(auto&& ids) {
    nb::list list;
    for (const ObjectId id : ids) {
        list.append(id.value());
    }
    return list;
}

/// An object id from an int or a `stresslab.Object`; invalid for None.
ObjectId to_object_id(nb::handle object) {
    if (object.is_none()) {
        return {};
    }
    if (nb::isinstance<PyModelObject>(object)) {
        return nb::cast<const PyModelObject&>(object).id;
    }
    const std::int64_t value = nb::cast<std::int64_t>(object);
    if (value < 0) {
        throw nb::value_error("object ids are positive");
    }
    return ObjectId(static_cast<std::uint64_t>(value));
}

/// What `stresslab.cmd` returns: the task of a command that goes on in the background, otherwise
/// its value.
nb::object result_to_python(const command::CommandResult& result) {
    if (result.task != nullptr) {
        return task_to_python(result.task);
    }
    return from_value(result.value);
}

command::CommandResult result_from_python(nb::handle object) {
    if (std::shared_ptr<task::Task> task = task_from_python(object)) {
        command::CommandResult result;
        result.task = std::move(task);
        return result;
    }
    return to_value(object);
}

nb::dict units_to_python(const model::UnitSystem& units) {
    nb::dict dict;
    dict["length"] = to_str(model::symbol(units.length));
    dict["mass"] = to_str(model::symbol(units.mass));
    dict["time"] = to_str(model::symbol(units.time));
    dict["temperature"] = to_str(model::symbol(units.temperature));
    return dict;
}

std::string object_repr(const PyModelObject& self) {
    std::string text = "<stresslab.Object " + model::to_string(self.id);
    if (!self.handle->is_open()) {
        return text + " (closed)>";
    }
    const model::Object* object = self.handle->document().find(self.id);
    if (object == nullptr) {
        return text + " (removed)>";
    }
    text += " " + object->type();
    if (!object->name().empty()) {
        text += " '" + object->name() + "'";
    }
    return text + ">";
}

// --- Transactions --------------------------------------------------------------------------------

/// An open transaction of Python. Rolled back when the document closes.
class TransactionState : public PythonResource {
public:
    explicit TransactionState(command::Transaction transaction)
        : transaction_(std::move(transaction)) {}

    [[nodiscard]] command::Transaction& get() {
        if (!transaction_) {
            throw ClosedError("the transaction is closed");
        }
        return *transaction_;
    }
    [[nodiscard]] bool is_open() const noexcept { return transaction_ && transaction_->is_open(); }

protected:
    void on_release() noexcept override { transaction_.reset(); }

private:
    std::optional<command::Transaction> transaction_;
};

struct PyTransaction {
    std::shared_ptr<DocumentHandle> handle;  // Keeps a document Python made alive.
    std::shared_ptr<TransactionState> state;
    std::string label;
};

// --- Commands registered from Python -------------------------------------------------------------

/// A command whose code is Python. The bus owns it through the handler, so it goes when the
/// command is unregistered for any reason; when the document closes, it unregisters itself.
class PythonCommand : public PythonResource {
public:
    PythonCommand(command::CommandBus& bus, std::string id, nb::object handler)
        : bus_(bus), id_(std::move(id)), handler_(std::move(handler)) {}

    [[nodiscard]] const PyRef& handler() const noexcept { return handler_; }

protected:
    void on_release() noexcept override {
        handler_.clear();
        bus_.unregister_command(id_);
    }

private:
    command::CommandBus& bus_;
    std::string id_;
    PyRef handler_;
};

/// `stresslab.CommandContext`: what the Python code of a command gets. Valid while it runs.
struct PyCommandContext {
    struct State {
        command::CommandContext* context = nullptr;
        std::weak_ptr<DocumentHandle> handle;
    };
    std::shared_ptr<State> state;

    [[nodiscard]] command::CommandContext& get() const {
        if (state->context == nullptr) {
            throw ClosedError("the command has finished; its context is no longer valid");
        }
        return *state->context;
    }
    [[nodiscard]] std::shared_ptr<DocumentHandle> handle() const {
        std::shared_ptr<DocumentHandle> handle = state->handle.lock();
        if (handle == nullptr || !handle->is_open()) {
            throw ClosedError("the document is closed");
        }
        return handle;
    }
};

command::CommandHandler python_handler(const std::shared_ptr<PythonCommand>& command,
                                       std::weak_ptr<DocumentHandle> handle) {
    return [command, handle = std::move(handle)](command::CommandContext& context) {
        const nb::gil_scoped_acquire gil;
        auto state = std::make_shared<PyCommandContext::State>();
        state->context = &context;
        state->handle = handle;
        struct Invalidate {
            PyCommandContext::State& state;
            ~Invalidate() { state.context = nullptr; }
        } invalidate{*state};
        const nb::object result = command->handler().get()(
            PyCommandContext{state}, **from_value_dict(context.args().values()));
        return result_from_python(result);
    };
}

ArgSpec make_arg_spec(std::string name, std::string_view type, bool required,
                      const nb::object& default_value) {
    const std::optional<command::ArgType> parsed = command::parse_arg_type(type);
    if (!parsed) {
        throw nb::value_error(("unknown argument type '" + std::string(type) + "'").c_str());
    }
    ArgSpec spec{std::move(name), *parsed, required, std::nullopt};
    if (!default_value.is_none()) {
        spec.default_value = to_value(default_value);
    }
    return spec;
}

}  // namespace

nb::object wrap_subscription(std::shared_ptr<PySubscription> subscription) {
    return nb::cast(PySubscription::Ref{std::move(subscription)});
}

void define_documents(nb::module_& m) {
    // --- Subscription ----------------------------------------------------------------------------

    nb::class_<PySubscription::Ref>(m, "Subscription",
                                    "Keeps a listener connected until `close` (or the end of a "
                                    "`with` block).")
        .def_prop_ro(
            "active",
            [](const PySubscription::Ref& self) { return !self.subscription->is_released(); })
        .def("close", [](PySubscription::Ref& self) { self.subscription->release(); })
        .def("__enter__", [](nb::handle self) { return nb::borrow(self); })
        .def("__exit__", [](PySubscription::Ref& self, nb::args) { self.subscription->release(); });

    // --- ModelChange -----------------------------------------------------------------------------

    nb::class_<command::ModelChange>(m, "ModelChange", "A completed step that changed the model.")
        .def_prop_ro("cause",
                     [](const command::ModelChange& self) {
                         return std::string(command::to_string(self.cause));
                     })
        .def_ro("label", &command::ModelChange::label)
        .def_prop_ro("created",
                     [](const command::ModelChange& self) { return ids_of(self.summary.created); })
        .def_prop_ro("removed",
                     [](const command::ModelChange& self) { return ids_of(self.summary.removed); })
        .def_prop_ro("updated",
                     [](const command::ModelChange& self) { return ids_of(self.summary.updated); })
        .def_prop_ro("units", [](const command::ModelChange& self) { return self.summary.units; });

    // --- Command specs ---------------------------------------------------------------------------

    nb::class_<ArgSpec>(m, "ArgSpec", "A keyword argument of a command.")
        .def(
            "__init__",
            [](ArgSpec* self, std::string name, std::string_view type, bool required,
               const nb::object& default_value) {
                new (self) ArgSpec(make_arg_spec(std::move(name), type, required, default_value));
            },
            nb::arg("name"), nb::arg("type") = "any", nb::kw_only(), nb::arg("required") = true,
            nb::arg("default").none() = nb::none())
        .def_ro("name", &ArgSpec::name)
        .def_prop_ro("type",
                     [](const ArgSpec& self) { return std::string(command::to_string(self.type)); })
        .def_ro("required", &ArgSpec::required)
        .def_prop_ro("has_default",
                     [](const ArgSpec& self) { return self.default_value.has_value(); })
        .def_prop_ro("default",
                     [](const ArgSpec& self) {
                         return self.default_value ? from_value(*self.default_value) : nb::none();
                     })
        .def("__repr__", [](const ArgSpec& self) {
            return "<stresslab.ArgSpec " + self.name + ": " +
                   std::string(command::to_string(self.type)) +
                   (self.required ? "" : " (optional)") + ">";
        });

    nb::class_<CommandSpec>(m, "CommandSpec", "A registered command: id, owner and arguments.")
        .def_ro("id", &CommandSpec::id)
        .def_ro("owner", &CommandSpec::owner)
        .def_ro("args", &CommandSpec::args)
        .def("__repr__",
             [](const CommandSpec& self) { return "<stresslab.CommandSpec " + self.id + ">"; });

    // --- Transaction -----------------------------------------------------------------------------

    nb::class_<PyTransaction>(m, "Transaction",
                              "Commands undone as one step. As a `with` block it commits when the "
                              "block ends and rolls back if the block raises.")
        .def_ro("label", &PyTransaction::label)
        .def_prop_ro("is_open", [](const PyTransaction& self) { return self.state->is_open(); })
        .def("commit", [](PyTransaction& self) { self.state->get().commit(); })
        .def("rollback", [](PyTransaction& self) { self.state->get().rollback(); })
        .def("__enter__", [](nb::handle self) { return nb::borrow(self); })
        .def(
            "__exit__",
            [](PyTransaction& self, nb::handle type, nb::handle, nb::handle) {
                if (!self.state->is_open()) {
                    return false;
                }
                if (type.is_none()) {
                    self.state->get().commit();
                } else {
                    self.state->get().rollback();
                }
                return false;
            },
            nb::arg().none(), nb::arg().none(), nb::arg().none());

    // --- CommandContext --------------------------------------------------------------------------

    nb::class_<PyCommandContext>(m, "CommandContext",
                                 "What the Python code of a command gets while it runs.")
        .def_prop_ro("spec",
                     [](const PyCommandContext& self) -> CommandSpec { return self.get().spec(); })
        .def_prop_ro("args",
                     [](const PyCommandContext& self) {
                         return from_value_dict(self.get().args().values());
                     })
        .def_prop_ro("document",
                     [](const PyCommandContext& self) {
                         (void)self.get();
                         return PyDocument{self.handle()};
                     })
        .def(
            "execute",
            [](const PyCommandContext& self, const std::string& command, const nb::kwargs& kwargs) {
                return result_to_python(self.get().execute(command, to_value_dict(kwargs)));
            },
            nb::arg(), nb::arg("kwargs"),
            "Runs another command as a part of this one: one undo step, one journal line.")
        .def(
            "create_object",
            [](const PyCommandContext& self, std::string type, std::string owner, std::string name,
               const nb::object& properties, const nb::object& parent,
               const nb::object& dependencies, std::uint32_t schema_version,
               const nb::object& data) {
                command::CommandContext& context = self.get();
                model::NewObject init;
                init.type = std::move(type);
                init.owner = std::move(owner);
                init.name = std::move(name);
                init.schema_version = schema_version;
                if (!properties.is_none()) {
                    init.properties = to_json(properties);
                }
                init.parent = to_object_id(parent);
                if (!dependencies.is_none()) {
                    for (nb::handle dependency : dependencies) {
                        init.dependencies.push_back(to_object_id(dependency));
                    }
                }
                if (!data.is_none()) {
                    init.data = data_from_python(data);
                }
                const ObjectId id = context.editor().create(std::move(init));
                return PyModelObject{self.handle(), id};
            },
            nb::arg("type"), nb::arg("owner"), nb::kw_only(), nb::arg("name") = "",
            nb::arg("properties").none() = nb::none(), nb::arg("parent").none() = nb::none(),
            nb::arg("dependencies").none() = nb::none(), nb::arg("schema_version") = 1,
            nb::arg("data").none() = nb::none(),
            "Creates an object, with bulk data (`stresslab.Mesh`, ...) if given.")
        .def(
            "set_data",
            [](const PyCommandContext& self, nb::handle object, nb::handle data) {
                self.get().editor().set_data(to_object_id(object), data_from_python(data));
            },
            nb::arg("object"), nb::arg("data"), "Replaces the bulk data of an object.");

    // --- Object ----------------------------------------------------------------------------------

    nb::class_<PyModelObject>(m, "Object",
                              "An object of a document, by id. Reading a removed object raises "
                              "ModelError.")
        .def_prop_ro("document", [](const PyModelObject& self) { return PyDocument{self.handle}; })
        .def_prop_ro("id", [](const PyModelObject& self) { return self.id.value(); })
        .def_prop_ro("exists",
                     [](const PyModelObject& self) {
                         return self.handle->is_open() && self.handle->document().contains(self.id);
                     })
        .def_prop_ro("type", [](const PyModelObject& self) { return self.get().type(); })
        .def_prop_ro("owner", [](const PyModelObject& self) { return self.get().owner(); })
        .def_prop_ro("schema_version",
                     [](const PyModelObject& self) { return self.get().schema_version(); })
        .def_prop_ro("name", [](const PyModelObject& self) { return self.get().name(); })
        .def_prop_ro("properties",
                     [](const PyModelObject& self) { return from_json(self.get().properties()); })
        .def_prop_ro("parent",
                     [](const PyModelObject& self) {
                         return object_or_none(self.handle, self.get().parent());
                     })
        .def_prop_ro("children",
                     [](const PyModelObject& self) {
                         return objects_of(self.handle, self.get().children());
                     })
        .def_prop_ro("dependencies",
                     [](const PyModelObject& self) {
                         return objects_of(self.handle, self.get().dependencies());
                     })
        .def_prop_ro("missing_dependencies",
                     [](const PyModelObject& self) {
                         return objects_of(self.handle,
                                           self.handle->document().missing_dependencies(self.id));
                     })
        .def_prop_ro("dependents",
                     [](const PyModelObject& self) {
                         return objects_of(self.handle,
                                           self.handle->document().dependents(self.id));
                     })
        .def_prop_ro(
            "is_opaque",
            [](const PyModelObject& self) { return self.handle->document().is_opaque(self.get()); })
        .def_prop_ro("data",
                     [](const PyModelObject& self) { return data_to_python(self.get().data()); })
        .def(
            "__eq__",
            [](const PyModelObject& self, nb::handle other) {
                if (!nb::isinstance<PyModelObject>(other)) {
                    return false;
                }
                const auto& that = nb::cast<const PyModelObject&>(other);
                return self.handle == that.handle && self.id == that.id;
            },
            nb::is_operator())
        .def("__hash__",
             [](const PyModelObject& self) {
                 return std::hash<const void*>{}(self.handle.get()) ^
                        std::hash<ObjectId>{}(self.id);
             })
        .def("__repr__", &object_repr);

    // --- Document --------------------------------------------------------------------------------

    nb::class_<PyDocument>(m, "Document",
                           "A model with its Command Bus. `Document()` makes a new one; "
                           "`stresslab.active_document()` is the one `stresslab.cmd` changes.")
        .def("__init__", [](PyDocument* self) { new (self) PyDocument{DocumentHandle::create()}; })
        .def_prop_ro("is_open", [](const PyDocument& self) { return self.handle->is_open(); })
        .def_prop_ro("is_attached",
                     [](const PyDocument& self) {
                         return self.handle == Session::instance().attached_document();
                     })
        .def_prop_ro(
            "units",
            [](const PyDocument& self) { return units_to_python(self.handle->document().units()); })
        .def(
            "set_owner_enabled",
            [](const PyDocument& self, std::string_view owner, bool enabled) {
                self.handle->owned_document().set_owner_enabled(owner, enabled);
            },
            nb::arg("owner"), nb::arg("enabled") = true,
            "Makes the objects of a module editable (or opaque again) in a document made in "
            "Python. Not a change of the model; the application enables modules of its own "
            "document.")
        .def(
            "is_owner_enabled",
            [](const PyDocument& self, std::string_view owner) {
                return self.handle->document().is_owner_enabled(owner);
            },
            nb::arg("owner"))
        // Commands
        .def(
            "cmd",
            [](const PyDocument& self, const std::string& command, const nb::kwargs& kwargs) {
                return result_to_python(self.handle->bus().execute(command, to_value_dict(kwargs)));
            },
            nb::arg(), nb::arg("kwargs"),
            "Runs a command; returns its value, or its Task if it goes on in the background.")
        .def(
            "format_call",
            [](const PyDocument& self, const std::string& command, const nb::kwargs& kwargs) {
                return self.handle->bus().format_call(command, to_value_dict(kwargs));
            },
            nb::arg(), nb::arg("kwargs"),
            "The journal line of a call, after its arguments are checked.")
        .def("command_ids", [](const PyDocument& self) { return self.handle->bus().command_ids(); })
        .def(
            "command_spec",
            [](const PyDocument& self, std::string_view id) -> nb::object {
                const CommandSpec* spec = self.handle->bus().find_command(id);
                return spec != nullptr ? nb::cast(*spec, nb::rv_policy::copy) : nb::none();
            },
            nb::arg("id"))
        .def(
            "register_command",
            [](const PyDocument& self, std::string id, nb::object handler, std::string owner,
               std::vector<ArgSpec> args) {
                if (!PyCallable_Check(handler.ptr())) {
                    throw nb::type_error("the handler of a command must be callable");
                }
                command::CommandBus& bus = self.handle->bus();
                auto command = std::make_shared<PythonCommand>(bus, id, std::move(handler));
                bus.register_command(CommandSpec{std::move(id), std::move(owner), std::move(args)},
                                     python_handler(command, self.handle));
                self.handle->resources().keep(command);
            },
            nb::arg("id"), nb::arg("handler"), nb::kw_only(), nb::arg("owner"),
            nb::arg("args") = std::vector<ArgSpec>{},
            "Registers a command whose code is `handler(context, **arguments)`.")
        .def(
            "unregister_command",
            [](const PyDocument& self, std::string_view id) {
                return self.handle->bus().unregister_command(id);
            },
            nb::arg("id"))
        // History
        .def("undo", [](const PyDocument& self) { return self.handle->bus().undo(); })
        .def("redo", [](const PyDocument& self) { return self.handle->bus().redo(); })
        .def_prop_ro("can_undo",
                     [](const PyDocument& self) { return self.handle->bus().can_undo(); })
        .def_prop_ro("can_redo",
                     [](const PyDocument& self) { return self.handle->bus().can_redo(); })
        .def_prop_ro(
            "undo_label",
            [](const PyDocument& self) { return std::string(self.handle->bus().undo_label()); })
        .def_prop_ro(
            "redo_label",
            [](const PyDocument& self) { return std::string(self.handle->bus().redo_label()); })
        .def(
            "transaction",
            [](const PyDocument& self, std::string label) {
                auto state =
                    std::make_shared<TransactionState>(self.handle->bus().begin_transaction(label));
                self.handle->resources().keep(state);
                return PyTransaction{self.handle, std::move(state), std::move(label)};
            },
            nb::arg("label") = "")
        .def_prop_ro("in_transaction",
                     [](const PyDocument& self) { return self.handle->bus().in_transaction(); })
        // Events
        .def(
            "on_journal",
            [](const PyDocument& self, nb::object listener) {
                PyRef ref(std::move(listener));
                auto subscription = std::make_shared<PySubscription>(
                    self.handle->bus().on_journal([ref](std::string_view entry) {
                        call_listener(ref, "journal listener", [&] { return to_str(entry); });
                    }),
                    false);
                self.handle->resources().hold(subscription);
                return wrap_subscription(std::move(subscription));
            },
            nb::arg("listener"),
            "Calls `listener(entry)` with every journal entry (lines without the final break).")
        .def(
            "on_model_changed",
            [](const PyDocument& self, nb::object listener) {
                PyRef ref(std::move(listener));
                auto subscription = std::make_shared<PySubscription>(
                    self.handle->bus().on_model_changed([ref](const command::ModelChange& change) {
                        call_listener(ref, "model change listener",
                                      [&] { return nb::cast(change, nb::rv_policy::copy); });
                    }),
                    false);
                self.handle->resources().hold(subscription);
                return wrap_subscription(std::move(subscription));
            },
            nb::arg("listener"),
            "Calls `listener(change)` after every step that changed the model.")
        // Objects
        .def("__len__", [](const PyDocument& self) { return self.handle->document().size(); })
        .def("__contains__",
             [](const PyDocument& self, nb::handle object) {
                 return self.handle->document().contains(to_object_id(object));
             })
        .def("__getitem__",
             [](const PyDocument& self, nb::handle object) {
                 const ObjectId id = to_object_id(object);
                 if (!self.handle->document().contains(id)) {
                     throw nb::key_error(model::to_string(id).c_str());
                 }
                 return PyModelObject{self.handle, id};
             })
        .def(
            "find",
            [](const PyDocument& self, nb::handle object) {
                const ObjectId id = to_object_id(object);
                return self.handle->document().contains(id) ? object_or_none(self.handle, id)
                                                            : nb::none();
            },
            nb::arg("id"), "The object with this id, or None.")
        .def("objects",
             [](const PyDocument& self) {
                 nb::list list;
                 for (const model::Object& object : self.handle->document().objects()) {
                     list.append(PyModelObject{self.handle, object.id()});
                 }
                 return list;
             })
        .def("roots",
             [](const PyDocument& self) {
                 return objects_of(self.handle, self.handle->document().roots());
             })
        .def(
            "find_by_type",
            [](const PyDocument& self, std::string_view type) {
                return objects_of(self.handle, self.handle->document().find_by_type(type));
            },
            nb::arg("type"))
        .def(
            "find_by_owner",
            [](const PyDocument& self, std::string_view owner) {
                return objects_of(self.handle, self.handle->document().find_by_owner(owner));
            },
            nb::arg("owner"))
        .def(
            "find_child",
            [](const PyDocument& self, nb::handle parent, std::string_view type,
               std::string_view name) {
                return object_or_none(self.handle, self.handle->document().find_child(
                                                       to_object_id(parent), type, name));
            },
            nb::arg("parent").none(), nb::arg("type"), nb::arg("name"),
            "The child of `parent` (None: a root object) with this type and name, or None.")
        .def("owners", [](const PyDocument& self) { return self.handle->document().owners(); })
        .def(
            "__eq__",
            [](const PyDocument& self, nb::handle other) {
                return nb::isinstance<PyDocument>(other) &&
                       nb::cast<const PyDocument&>(other).handle == self.handle;
            },
            nb::is_operator())
        .def("__hash__",
             [](const PyDocument& self) { return std::hash<const void*>{}(self.handle.get()); })
        .def("__repr__", [](const PyDocument& self) {
            if (!self.handle->is_open()) {
                return std::string("<stresslab.Document (closed)>");
            }
            return "<stresslab.Document with " + std::to_string(self.handle->document().size()) +
                   " objects>";
        });

    // --- Module functions ------------------------------------------------------------------------

    m.def(
        "active_document", [] { return PyDocument{Session::instance().active_document()}; },
        "The document `stresslab.cmd` changes: the document of the application, or a new one.");
    m.def(
        "set_active_document",
        [](const PyDocument& document) {
            Session::instance().set_active_document(document.handle);
        },
        nb::arg("document"));
    m.def(
        "attached_document",
        []() -> nb::object {
            std::shared_ptr<DocumentHandle> attached = Session::instance().attached_document();
            return attached != nullptr ? nb::cast(PyDocument{std::move(attached)}) : nb::none();
        },
        "The document of the application that embeds Python, or None in a plain Python process.");
    m.def("journal_header", &command::journal_header,
          "The lines every session journal (.slpy) starts with.");
    m.def(
        "_shutdown",
        [] {
            Session::instance().shutdown();
            // Objects in reference cycles (tracebacks of listeners, for instance) go now, while
            // the module they belong to is still there.
            nb::module_::import_("gc").attr("collect")();
        },
        "Releases what Python keeps in the core; called at exit.");
}

}  // namespace fem::python
