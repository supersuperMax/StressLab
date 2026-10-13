#include "python_test_support.h"

#include <gtest/gtest.h>

namespace fem::python::testing {

namespace {

Interpreter* python = nullptr;

class PythonEnvironment : public ::testing::Environment {
public:
    void SetUp() override { python = new Interpreter(); }
    void TearDown() override {
        delete python;
        python = nullptr;
    }
};

[[maybe_unused]] const ::testing::Environment* const environment =
    ::testing::AddGlobalTestEnvironment(new PythonEnvironment());

}  // namespace

Interpreter& interpreter() {
    return *python;
}

}  // namespace fem::python::testing
