#include "fem/storage/session.h"

#include "common.h"

namespace fem::python {

namespace {

storage::ProjectSession& project() {
    return Session::instance().active_document()->project();
}

}  // namespace

void define_project(nb::module_& m) {
    nb::module_ project_module = m.def_submodule("project");
    project_module.def(
        "open", [](nb::handle folder) { project().open(to_path(folder)); }, nb::arg("folder"),
        "Opens the project folder in place of the active document. Written to the journal.");
    project_module.def(
        "save", [] { project().save(); },
        "Saves the active document to its project folder; ProjectError (code \"no_folder\") if "
        "it has none yet. Written to the journal.");
    project_module.def(
        "save_as", [](nb::handle folder) { project().save_as(to_path(folder)); }, nb::arg("folder"),
        "Saves the active document to the folder, which becomes its project folder; \".slproj\" "
        "is added to a name without it. Written to the journal.");
    project_module.def(
        "clean",
        [] {
            const storage::CleanStats stats = project().clean();
            nb::dict result;
            result["removed"] = stats.removed;
            result["freed_bytes"] = stats.freed_bytes;
            result["kept"] = stats.kept;
            result["kept_bytes"] = stats.kept_bytes;
            return result;
        },
        "Removes the blocks of bulk data the saved project no longer refers to (\"Clean "
        "project\"); returns {\"removed\", \"freed_bytes\", \"kept\", \"kept_bytes\"}. Written to "
        "the journal.");
    const auto stats = [](const storage::PackStats& packed) {
        nb::dict result;
        result["files"] = packed.files;
        result["bytes"] = packed.bytes;
        result["package_bytes"] = packed.package_bytes;
        return result;
    };
    project_module.def(
        "pack",
        [stats](nb::handle package, bool history) {
            return stats(project().pack(to_path(package), history));
        },
        nb::arg("package"), nb::arg("history") = true,
        "Saves the project and packs it into one .slpack file (\".slpack\" is added to a name "
        "without it), with its history or without it; returns {\"files\", \"bytes\", "
        "\"package_bytes\"}. Written to the journal.");
    project_module.def(
        "unpack",
        [stats](nb::handle package, nb::handle folder) {
            return stats(project().unpack(to_path(package), to_path(folder)));
        },
        nb::arg("package"), nb::arg("folder"),
        "Unpacks a .slpack into a new or empty folder (\".slproj\" is added to a name without it) "
        "and opens that project in place of the active document. Written to the journal.");
    project_module.def(
        "close", [] { project().close(); },
        "Replaces the active document with a new empty one without a folder. Written to the "
        "journal.");
    project_module.def(
        "folder",
        [] {
            const auto& folder = project().folder();
            return folder ? to_python_path(*folder) : nb::none();
        },
        "The project folder of the active document as a pathlib.Path; None if it was never "
        "saved.");
    project_module.def(
        "is_modified", [] { return project().is_modified(); },
        "True if the model changed since the project was opened, saved or closed.");
}

}  // namespace fem::python
