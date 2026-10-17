#pragma once

// Shared by the binding sources of the `stresslab` module. Python.h comes first, through nanobind.
#include "fem/core/command/value.h"
#include "fem/core/context/value.h"
#include "fem/core/model/object.h"
#include "fem/core/model/object_id.h"
#include "fem/core/task/task.h"
#include "fem/python/session.h"

#include <nanobind/nanobind.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace fem::python {

namespace nb = nanobind;

// --- Python objects kept by the core -------------------------------------------------------------

/// A Python object that C++ code keeps and may drop on any thread: the reference is dropped with
/// the GIL taken. After Python has shut down the reference is leaked instead.
class PyRef {
public:
    PyRef() = default;
    explicit PyRef(nb::object object) : holder_(std::make_shared<Holder>(std::move(object))) {}

    /// The object; throws ClosedError once it has been cleared.
    [[nodiscard]] nb::object get() const;
    /// Drops the reference now (needs the GIL); later `get` throws.
    void clear() noexcept;

private:
    struct Holder {
        explicit Holder(nb::object object) : object(std::move(object)) {}
        Holder(const Holder&) = delete;
        Holder& operator=(const Holder&) = delete;
        ~Holder();

        nb::object object;
    };

    std::shared_ptr<Holder> holder_;
};

/// Calls a Python listener from the core, on any thread. `build` makes the argument while the GIL
/// is held. What the listener raises is reported as unraisable (printed to stderr) and does not
/// reach the code that changed the model, the keys or the task.
template <class Build>
void call_listener(const PyRef& listener, const char* where, Build&& build) noexcept {
    const nb::gil_scoped_acquire gil;
    try {
        listener.get()(build());
    } catch (nb::python_error& error) {
        error.discard_as_unraisable(where);
    } catch (const std::exception& error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
        PyErr_WriteUnraisable(nb::str(where).ptr());
    }
}

// --- What Python holds ---------------------------------------------------------------------------

/// `stresslab.Document`: a handle to a document. Copies refer to the same document.
struct PyDocument {
    std::shared_ptr<DocumentHandle> handle;
};

/// `stresslab.Object`: an object of a document by its id. It may refer to a removed object; then
/// reading it raises ModelError.
struct PyModelObject {
    std::shared_ptr<DocumentHandle> handle;
    model::ObjectId id;

    [[nodiscard]] const model::Object& get() const { return handle->document().get(id); }
};

/// `stresslab.Subscription`: keeps a listener connected until closed, or until its document
/// closes or Python shuts down. Dropping the Python object does not disconnect the listener.
class PySubscription : public PythonResource {
public:
    /// `concurrent`: the listener may be called on another thread, so the GIL is released while
    /// waiting for a call in progress.
    PySubscription(Subscription subscription, bool concurrent)
        : subscription_(std::move(subscription)), concurrent_(concurrent) {}

    /// The Python side: `stresslab.Subscription` refers to it.
    struct Ref {
        std::shared_ptr<PySubscription> subscription;
    };

protected:
    void on_release() noexcept override;

private:
    Subscription subscription_;
    bool concurrent_;
};

/// `stresslab.Subscription` for `subscription`.
[[nodiscard]] nb::object wrap_subscription(std::shared_ptr<PySubscription> subscription);

// --- Values --------------------------------------------------------------------------------------

/// A Python value as an argument of a command: None, bool, int, float, str, os.PathLike (as a
/// path), list, tuple, dict with str keys and `stresslab.Object` (as its id). Integers of other
/// types (numpy) are accepted through `__index__`. Raises TypeError for anything else.
[[nodiscard]] command::Value to_value(nb::handle object);
/// Keyword arguments as a dictionary of values.
[[nodiscard]] command::Value::Dict to_value_dict(nb::handle kwargs);
/// A value as Python: paths become `pathlib.Path`.
[[nodiscard]] nb::object from_value(const command::Value& value);
[[nodiscard]] nb::dict from_value_dict(const command::Value::Dict& values);

/// Properties of an object as Python: a dict with JSON values.
[[nodiscard]] nb::object from_json(const nlohmann::json& value);
/// A Python value as properties: None, bool, int, float, str, list, tuple and dict with str keys.
[[nodiscard]] nlohmann::json to_json(nb::handle object);

/// Context key values: None, bool, int or float (a number), str, a list of str.
[[nodiscard]] context::ContextValue to_context_value(nb::handle object);
[[nodiscard]] nb::object from_context_value(const context::ContextValue& value);

/// UTF-8 text of a Python str; raises TypeError for other types.
[[nodiscard]] std::string to_utf8(nb::handle object);
[[nodiscard]] nb::str to_str(std::string_view text);
[[nodiscard]] nb::object to_python_path(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path to_path(nb::handle object);

// --- Parts of the module -------------------------------------------------------------------------

void define_errors(nb::module_& m);
void define_documents(nb::module_& m);
void define_mesh(nb::module_& m);
void define_context(nb::module_& m);
void define_tasks(nb::module_& m);
void define_extensions(nb::module_& m);
void define_project(nb::module_& m);
void define_history(nb::module_& m);
void define_results(nb::module_& m);

/// Data of an object as its Python view (`stresslab.Mesh`, ...), or None.
[[nodiscard]] nb::object data_to_python(const std::shared_ptr<const model::ObjectData>& data);
/// The data a Python view holds; raises TypeError for other objects.
[[nodiscard]] std::shared_ptr<const model::ObjectData> data_from_python(nb::handle object);

/// `stresslab.Task` for a task of the core.
[[nodiscard]] nb::object task_to_python(std::shared_ptr<task::Task> task);
/// The task a `stresslab.Task` refers to, or nullptr if `object` is not one.
[[nodiscard]] std::shared_ptr<task::Task> task_from_python(nb::handle object);

}  // namespace fem::python
