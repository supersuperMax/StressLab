// The entry point of the native module `_stresslab`. Compiled into the extension module for plain
// Python and into every executable that embeds Python, where it is a built-in module.
#include <nanobind/nanobind.h>

namespace fem::python {
void define_module(nanobind::module_& m);
}  // namespace fem::python

NB_MODULE(_stresslab, m) {
    fem::python::define_module(m);
}
