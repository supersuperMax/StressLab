#include "fem/core/context/context_keys.h"
#include "fem/core/context/error.h"
#include "fem/core/context/when.h"

#include "common.h"
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fem::python {

namespace {

/// `stresslab.context`: the context keys of the session, looked up on every call, so that it
/// follows the keys the application attaches.
struct PyContext {};

context::ContextKeys& keys() {
    return Session::instance().context();
}

context::ContextValue::Kind to_kind(std::string_view name) {
    using Kind = context::ContextValue::Kind;
    for (const Kind kind : {Kind::boolean, Kind::number, Kind::string, Kind::list}) {
        if (context::to_string(kind) == name) {
            return kind;
        }
    }
    throw nb::value_error(("unknown context key type '" + std::string(name) + "'").c_str());
}

nb::dict spec_to_python(const context::KeySpec& spec) {
    nb::dict dict;
    dict["name"] = to_str(spec.name);
    dict["owner"] = to_str(spec.owner);
    dict["type"] = to_str(context::to_string(spec.type));
    dict["initial"] = from_context_value(spec.initial);
    return dict;
}

}  // namespace

void define_context(nb::module_& m) {
    nb::class_<PyContext>(m, "Context",
                          "Context keys: what `when` conditions of menus and panels read "
                          "(docs/architecture.md §4.3).")
        .def("__getitem__",
             [](const PyContext&, std::string_view key) {
                 return from_context_value(keys().value(key));
             })
        .def(
            "get",
            [](const PyContext&, std::string_view key, nb::object fallback) {
                const context::ContextValue& value = keys().value(key);
                return value.is_none() ? std::move(fallback) : from_context_value(value);
            },
            nb::arg("key"), nb::arg("default").none() = nb::none(),
            "The value of a key; `default` if it is not declared or has no value.")
        .def(
            "__setitem__",
            [](const PyContext&, std::string_view key, nb::handle value) {
                keys().set(key, to_context_value(value));
            },
            nb::arg("key"), nb::arg("value").none())
        .def(
            "set",
            [](const PyContext&, std::string_view key, nb::handle value) {
                keys().set(key, to_context_value(value));
            },
            nb::arg("key"), nb::arg("value").none(), "Sets a declared key; None clears it.")
        .def(
            "update",
            [](const PyContext&, const nb::dict& values) {
                std::vector<std::pair<std::string, context::ContextValue>> changes;
                for (auto [key, value] : values) {
                    changes.emplace_back(to_utf8(key), to_context_value(value));
                }
                keys().set(std::move(changes));
            },
            nb::arg("values"), "Sets several keys at once: watchers see all new values together.")
        .def(
            "declare",
            [](const PyContext&, std::string name, std::string owner, std::string_view type,
               nb::handle initial) {
                return keys().declare(context::KeySpec{std::move(name), std::move(owner),
                                                       to_kind(type), to_context_value(initial)});
            },
            nb::arg("name"), nb::kw_only(), nb::arg("owner"), nb::arg("type"),
            nb::arg("initial").none() = nb::none(),
            "Declares a key: type is 'boolean', 'number', 'string' or 'list'. False if it was "
            "declared already with the same spec.")
        .def(
            "remove_owner",
            [](const PyContext&, std::string_view owner) { return keys().remove_owner(owner); },
            nb::arg("owner"), "Removes every key of a module; returns how many.")
        .def(
            "keys", [](const PyContext&) { return keys().keys(); }, "Declared keys, sorted.")
        .def("__contains__",
             [](const PyContext&, std::string_view key) { return keys().find(key) != nullptr; })
        .def(
            "spec",
            [](const PyContext&, std::string_view key) -> nb::object {
                const context::KeySpec* spec = keys().find(key);
                return spec != nullptr ? nb::object(spec_to_python(*spec)) : nb::none();
            },
            nb::arg("key"), "The declaration of a key as a dict, or None.")
        .def(
            "evaluate",
            [](const PyContext&, std::string_view expression) {
                return keys().evaluate(context::WhenExpression::parse(expression));
            },
            nb::arg("expression"), "The value of a `when` expression now.")
        .def(
            "watch",
            [](const PyContext&, std::string_view expression, nb::object listener) {
                if (!PyCallable_Check(listener.ptr())) {
                    throw nb::type_error("the listener must be callable");
                }
                PyRef ref(std::move(listener));
                auto subscription = std::make_shared<PySubscription>(
                    keys().watch(context::WhenExpression::parse(expression),
                                 [ref](bool value) {
                                     call_listener(ref, "context watch listener",
                                                   [&] { return nb::bool_(value); });
                                 }),
                    false);
                Session::instance().resources().hold(subscription);
                return wrap_subscription(std::move(subscription));
            },
            nb::arg("expression"), nb::arg("listener"),
            "Calls `listener(value)` every time the value of the expression changes.");

    m.attr("context") = PyContext{};
}

}  // namespace fem::python
