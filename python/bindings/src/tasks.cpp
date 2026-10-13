#include "fem/core/task/error.h"
#include "fem/core/task/task.h"

#include "common.h"
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace fem::python {

// Members that may notify listeners run without the GIL: a listener called on a worker thread
// holds the listener's lock while it waits for the GIL.
namespace {

using task::MessageSeverity;
using task::Task;
using task::TaskControl;

struct TaskRef {
    std::shared_ptr<Task> task;
};

nb::object progress_to_python(const std::optional<double>& progress) {
    return progress ? nb::object(nb::float_(*progress)) : nb::none();
}

nb::tuple message_to_python(const task::TaskMessage& message) {
    return nb::make_tuple(to_str(task::to_string(message.severity)), to_str(message.text));
}

MessageSeverity to_severity(std::string_view name) {
    for (const MessageSeverity severity :
         {MessageSeverity::info, MessageSeverity::warning, MessageSeverity::error}) {
        if (task::to_string(severity) == name) {
            return severity;
        }
    }
    throw nb::value_error(("unknown message severity '" + std::string(name) + "'").c_str());
}

/// How long a wait goes without looking at signals and pending calls.
constexpr std::chrono::milliseconds wait_slice{50};

/// Waits for the task without holding the GIL; false if the time ran out. Waits in slices and
/// between them lets Python handle signals (Ctrl+C) and run pending calls: the application may
/// have work for this thread that the task waits for (a module's task that asks the model for
/// something, `Interpreter::call_soon`).
bool wait(const Task& task, const std::optional<double>& timeout) {
    using clock = std::chrono::steady_clock;
    const std::optional<clock::time_point> deadline =
        timeout ? std::optional(clock::now() + std::chrono::duration_cast<clock::duration>(
                                                   std::chrono::duration<double>(*timeout)))
                : std::nullopt;
    while (true) {
        auto slice = std::chrono::duration_cast<clock::duration>(wait_slice);
        if (deadline) {
            slice = std::max(clock::duration::zero(), std::min(slice, *deadline - clock::now()));
        }
        bool finished = false;
        {
            const nb::gil_scoped_release unlocked;
            finished = task.wait_for(slice);
        }
        if (finished) {
            return true;
        }
        if (deadline && clock::now() >= *deadline) {
            return false;
        }
        if (PyErr_CheckSignals() != 0 || Py_MakePendingCalls() != 0) {
            throw nb::python_error();
        }
    }
}

}  // namespace

nb::object task_to_python(std::shared_ptr<Task> task) {
    return nb::cast(TaskRef{std::move(task)});
}

std::shared_ptr<Task> task_from_python(nb::handle object) {
    if (!nb::isinstance<TaskRef>(object)) {
        return nullptr;
    }
    return nb::cast<const TaskRef&>(object).task;
}

void define_tasks(nb::module_& m) {
    nb::class_<task::TaskEvent>(m, "TaskEvent", "What changed in a task.")
        .def_prop_ro("kind",
                     [](const task::TaskEvent& self) -> std::string {
                         switch (self.kind) {
                             case task::TaskEvent::Kind::state:
                                 return "state";
                             case task::TaskEvent::Kind::progress:
                                 return "progress";
                             case task::TaskEvent::Kind::message:
                                 return "message";
                         }
                         return {};
                     })
        .def_prop_ro(
            "state",
            [](const task::TaskEvent& self) { return std::string(task::to_string(self.state)); })
        .def_prop_ro("progress",
                     [](const task::TaskEvent& self) { return progress_to_python(self.progress); })
        .def_prop_ro("message", [](const task::TaskEvent& self) -> nb::object {
            if (self.kind != task::TaskEvent::Kind::message) {
                return nb::none();
            }
            return message_to_python(self.message);
        });

    nb::class_<TaskRef>(m, "Task",
                        "A long operation running in the background: progress, messages, "
                        "cancellation and a result (docs/architecture.md §4.6, item 7).")
        .def_prop_ro("title", [](const TaskRef& self) { return self.task->title(); })
        .def_prop_ro(
            "state",
            [](const TaskRef& self) { return std::string(task::to_string(self.task->state())); },
            "'pending', 'running', 'succeeded', 'failed' or 'cancelled'.")
        .def_prop_ro("finished", [](const TaskRef& self) { return self.task->is_finished(); })
        .def_prop_ro(
            "progress",
            [](const TaskRef& self) { return progress_to_python(self.task->progress()); },
            "From 0 to 1, or None while unknown.")
        .def_prop_ro(
            "messages",
            [](const TaskRef& self) {
                nb::list list;
                for (const task::TaskMessage& message : self.task->messages()) {
                    list.append(message_to_python(message));
                }
                return list;
            },
            "(severity, text) pairs, oldest first.")
        .def_prop_ro("cancel_requested",
                     [](const TaskRef& self) { return self.task->is_cancel_requested(); })
        .def_prop_ro(
            "error",
            [](const TaskRef& self) -> nb::object {
                if (self.task->state() != task::TaskState::failed) {
                    return nb::none();
                }
                return to_str(self.task->error_message());
            },
            "The error message of a failed task, or None.")
        .def(
            "cancel", [](const TaskRef& self) { return self.task->cancel(); },
            nb::call_guard<nb::gil_scoped_release>(),
            "Asks the work to stop; False if the task has finished or was asked before.")
        .def(
            "wait",
            [](const TaskRef& self, std::optional<double> timeout) {
                return wait(*self.task, timeout);
            },
            nb::arg("timeout") = nb::none(),
            "Waits for the task to finish; False if `timeout` seconds ran out first.")
        .def(
            "result",
            [](const TaskRef& self, std::optional<double> timeout) {
                wait(*self.task, timeout);
                return from_value(self.task->result());
            },
            nb::arg("timeout") = nb::none(),
            "Waits and returns the result. Raises TaskCancelled, the error of a failed task, or "
            "TaskError if the task has not finished within `timeout` seconds.")
        .def(
            "subscribe",
            [](const TaskRef& self, nb::object listener) {
                if (!PyCallable_Check(listener.ptr())) {
                    throw nb::type_error("the listener must be callable");
                }
                PyRef ref(std::move(listener));
                auto subscription = std::make_shared<PySubscription>(
                    self.task->subscribe([ref](const task::TaskEvent& event) {
                        call_listener(ref, "task listener",
                                      [&] { return nb::cast(event, nb::rv_policy::copy); });
                    }),
                    true);
                Session::instance().resources().hold(subscription);
                return wrap_subscription(std::move(subscription));
            },
            nb::arg("listener"),
            "Calls `listener(event)` after every change, on the thread that made it.")
        .def(
            "__eq__",
            [](const TaskRef& self, nb::handle other) {
                return nb::isinstance<TaskRef>(other) &&
                       nb::cast<const TaskRef&>(other).task == self.task;
            },
            nb::is_operator())
        .def("__hash__",
             [](const TaskRef& self) { return std::hash<const void*>{}(self.task.get()); })
        .def("__repr__", [](const TaskRef& self) {
            return "<stresslab.Task '" + self.task->title() + "' " +
                   std::string(task::to_string(self.task->state())) + ">";
        });

    nb::class_<TaskControl>(m, "TaskControl",
                            "The side of the work: starts the task, reports progress and "
                            "finishes it. May be used from any thread.")
        .def_prop_ro("task", [](const TaskControl& self) { return TaskRef{self.task()}; })
        .def("start", &TaskControl::start, nb::call_guard<nb::gil_scoped_release>(),
             "pending -> running; False if the task was cancelled while pending.")
        .def("set_progress", &TaskControl::set_progress, nb::arg("fraction"),
             nb::call_guard<nb::gil_scoped_release>())
        .def("set_indeterminate", &TaskControl::set_indeterminate,
             nb::call_guard<nb::gil_scoped_release>())
        .def(
            "message",
            [](TaskControl& self, std::string text, std::string_view severity) {
                const MessageSeverity parsed = to_severity(severity);
                const nb::gil_scoped_release unlocked;
                self.message(std::move(text), parsed);
            },
            nb::arg("text"), nb::arg("severity") = "info")
        .def_prop_ro("cancel_requested", &TaskControl::is_cancel_requested)
        .def("check_cancelled", &TaskControl::throw_if_cancel_requested,
             "Raises TaskCancelled if the task was asked to stop.")
        .def(
            "succeed",
            [](TaskControl& self, nb::handle value) {
                command::Value converted = to_value(value);
                const nb::gil_scoped_release unlocked;
                return self.succeed(std::move(converted));
            },
            nb::arg("value").none() = nb::none())
        .def(
            "fail",
            [](TaskControl& self, const std::string& message) { return self.fail(message); },
            nb::arg("message"), nb::call_guard<nb::gil_scoped_release>())
        .def("finish_cancelled", &TaskControl::finish_cancelled,
             nb::call_guard<nb::gil_scoped_release>());

    m.def(
        "make_task",
        [](std::string title) {
            TaskControl control = task::make_task(std::move(title));
            nb::object task = task_to_python(control.task());
            return nb::make_tuple(task, nb::cast(std::move(control)));
        },
        nb::arg("title"),
        "A new pending task and its control, for work the caller runs itself; see "
        "`stresslab.run_task`.");
}

}  // namespace fem::python
