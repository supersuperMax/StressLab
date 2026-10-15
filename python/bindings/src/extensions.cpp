#include "fem/core/extension/manager.h"
#include "fem/core/extension/manifest.h"

#include "common.h"
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fem::python {

namespace {

/// `stresslab.ExtensionInfo`: an installed module as it was when Python asked. The manager may
/// forget a module later (uninstalling it), so Python keeps a copy, not a reference.
struct PyExtensionInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string kind;
    std::string source;
    std::string state;
    std::filesystem::path folder;
};

PyExtensionInfo snapshot(const extension::ExtensionInfo& info) {
    return {.id = info.id(),
            .name = info.manifest->name,
            .version = info.manifest->version.to_string(),
            .kind = info.manifest->kind ? std::string(extension::to_string(*info.manifest->kind))
                                        : std::string(),
            .source = std::string(extension::to_string(info.source)),
            .state = std::string(extension::to_string(info.state)),
            .folder = info.folder};
}

extension::ExtensionManager& manager() {
    return Session::instance().extensions();
}

}  // namespace

void define_extensions(nb::module_& m) {
    nb::class_<PyExtensionInfo>(m, "ExtensionInfo",
                                "An installed module as it was when it was asked for "
                                "(`stresslab.extensions.get`).")
        .def_ro("id", &PyExtensionInfo::id)
        .def_ro("name", &PyExtensionInfo::name,
                "The name as the manifest writes it; \"%key%\" names a string of its catalogs.")
        .def_ro("version", &PyExtensionInfo::version)
        .def_ro("kind", &PyExtensionInfo::kind, "\"native\", \"python\", \"process\" or \"\".")
        .def_ro("source", &PyExtensionInfo::source, "\"builtin\", \"user\" or \"local\".")
        .def_ro("state", &PyExtensionInfo::state,
                "\"disabled\", \"enabled\" or \"active\" (its code is running).")
        .def_prop_ro("enabled",
                     [](const PyExtensionInfo& self) { return self.state != "disabled"; })
        .def_prop_ro("folder",
                     [](const PyExtensionInfo& self) { return to_python_path(self.folder); })
        .def("__repr__", [](const PyExtensionInfo& self) {
            return "<stresslab.ExtensionInfo " + self.id + " " + self.version + " " + self.state +
                   ">";
        });

    nb::module_ extensions = m.def_submodule("extensions");
    extensions.def(
        "installed",
        [] {
            std::vector<PyExtensionInfo> result;
            for (const extension::ExtensionInfo* info : manager().extensions()) {
                result.push_back(snapshot(*info));
            }
            return result;
        },
        "The installed modules, sorted by id.");
    extensions.def(
        "get", [](std::string_view id) { return snapshot(manager().get(id)); }, nb::arg("id"),
        "The installed module; ExtensionError if there is none.");
    extensions.def(
        "enable",
        [](std::string_view id) { return manager().enable(id, extension::Journaling::on); },
        nb::arg("id"),
        "Enables a module and the modules it requires, as the module manager does, and returns "
        "those it enabled, required first. The call is written to the journal; not allowed "
        "inside a command or a transaction.");
    extensions.def(
        "disable",
        [](std::string_view id) { return manager().disable(id, extension::Journaling::on); },
        nb::arg("id"),
        "Disables a module and the modules that need it, and returns those it disabled. "
        "Everything the module added to the window goes away; its objects stay in the model, "
        "opaque, until it is enabled again. The call is written to the journal; not allowed "
        "inside a command or a transaction.");
}

}  // namespace fem::python
