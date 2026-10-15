#include "fem/core/command/error.h"
#include "fem/core/context/error.h"
#include "fem/core/extension/error.h"
#include "fem/core/job/error.h"
#include "fem/core/l10n/error.h"
#include "fem/core/model/error.h"
#include "fem/core/settings/error.h"
#include "fem/core/task/error.h"

#include "common.h"

#include <string_view>

namespace fem::python {

namespace {

// Exception types of the module; they live as long as the interpreter.
struct ErrorTypes {
    PyObject* error = nullptr;
    PyObject* closed = nullptr;
    PyObject* command = nullptr;
    PyObject* model = nullptr;
    PyObject* context = nullptr;
    PyObject* when_syntax = nullptr;
    PyObject* task = nullptr;
    PyObject* task_cancelled = nullptr;
    PyObject* job = nullptr;
    PyObject* extension = nullptr;
};

ErrorTypes types;

PyObject* new_error(nb::module_& m, const char* name, PyObject* base, const char* doc) {
    const std::string qualified = std::string("stresslab.") + name;
    nb::dict attributes;
    attributes["__doc__"] = doc;
    attributes["code"] = nb::none();
    PyObject* type = PyErr_NewException(qualified.c_str(), base, attributes.ptr());
    if (type == nullptr) {
        throw nb::python_error();
    }
    m.attr(name) = nb::handle(type);
    return type;  // The module keeps a reference; this one is never released.
}

/// `type(message)` with `code` set, not raised yet; invalid if making it raised an error.
nb::object make_error(PyObject* type, const char* message, std::string_view code) {
    const nb::str text(message);
    nb::object error = nb::steal(PyObject_CallFunctionObjArgs(type, text.ptr(), nullptr));
    if (error.is_valid()) {
        error.attr("code") = to_str(code);
    }
    return error;
}

void raise(PyObject* type, const nb::object& error) {
    if (error.is_valid()) {
        PyErr_SetObject(type, error.ptr());
    }
}

void raise(PyObject* type, const char* message, std::string_view code) {
    raise(type, make_error(type, message, code));
}

void translate(const std::exception_ptr& exception, void* /*payload*/) {
    try {
        std::rethrow_exception(exception);
    } catch (const ClosedError& e) {
        raise(types.closed, e.what(), "closed");
    } catch (const command::CommandError& e) {
        raise(types.command, e.what(), command::to_string(e.code()));
    } catch (const model::ModelError& e) {
        raise(types.model, e.what(), model::to_string(e.code()));
    } catch (const context::WhenSyntaxError& e) {
        nb::object error = make_error(types.when_syntax, e.what(), context::to_string(e.code()));
        if (error.is_valid()) {
            error.attr("column") = e.column();
            error.attr("reason") = to_str(e.reason());
        }
        raise(types.when_syntax, error);
    } catch (const context::ContextError& e) {
        raise(types.context, e.what(), context::to_string(e.code()));
    } catch (const task::TaskCancelled& e) {
        raise(types.task_cancelled, e.what(), task::to_string(e.code()));
    } catch (const task::TaskError& e) {
        raise(types.task, e.what(), task::to_string(e.code()));
    } catch (const job::JobError& e) {
        raise(types.job, e.what(), job::to_string(e.code()));
    } catch (const extension::ExtensionError& e) {
        raise(types.extension, e.what(), extension::to_string(e.code()));
    } catch (const settings::SettingsError& e) {
        raise(types.error, e.what(), settings::to_string(e.code()));
    } catch (const l10n::L10nError& e) {
        raise(types.error, e.what(), l10n::to_string(e.code()));
    }
}

}  // namespace

void define_errors(nb::module_& m) {
    types.error = new_error(m, "Error", PyExc_RuntimeError,
                            "Base of the errors of StressLab. `code` names what went wrong.");
    types.closed = new_error(m, "ClosedError", types.error,
                             "The document or command is no longer available to Python.");
    types.command = new_error(m, "CommandError", types.error,
                              "The Command Bus refused a call: unknown command, bad arguments...");
    types.model =
        new_error(m, "ModelError", types.error, "The model refused a change or a lookup failed.");
    types.context = new_error(m, "ContextError", types.error, "Bad context key or value.");
    types.when_syntax =
        new_error(m, "WhenSyntaxError", types.context,
                  "A `when` expression cannot be parsed; see `column` and `reason`.");
    types.task = new_error(m, "TaskError", types.error, "A task failed or is not finished.");
    types.task_cancelled = new_error(m, "TaskCancelled", types.task, "The task was cancelled.");
    types.job = new_error(m, "JobError", types.error, "A job of the Job Manager failed.");
    types.extension = new_error(m, "ExtensionError", types.error,
                                "A module cannot be enabled, disabled or found, or its code "
                                "failed.");
    nb::register_exception_translator(&translate);
}

}  // namespace fem::python
