#include "fem/core/version.h"

#include "common.h"
#include <nanobind/stl/string.h>

#include <string>

namespace fem::python {

void define_module(nb::module_& m) {
    m.doc() = "Native part of the stresslab package; import stresslab instead.";
#ifdef NDEBUG
    // Objects still referenced when Python stops (from tracebacks kept to the end, for instance)
    // are reported by nanobind as leaks. Useful while developing, noise for users.
    nb::set_leak_warnings(false);
#endif
    m.attr("__version__") = std::string(fem::version());
    define_errors(m);
    define_documents(m);
    define_mesh(m);
    define_context(m);
    define_tasks(m);
    define_extensions(m);
    define_project(m);
    define_history(m);
    // What Python keeps in the core is released while Python can still destroy it.
    nb::module_::import_("atexit").attr("register")(m.attr("_shutdown"));
}

}  // namespace fem::python
