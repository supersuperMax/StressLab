#pragma once

#include "fem/python/interpreter.h"

namespace fem::python::testing {

/// The interpreter of the whole test program: Python cannot start twice in a process. Started
/// before the first test and stopped after the last one (python_test_support.cpp).
[[nodiscard]] Interpreter& interpreter();

}  // namespace fem::python::testing
