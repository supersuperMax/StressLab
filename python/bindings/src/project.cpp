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
