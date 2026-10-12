find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(
    fem_core_tests
    unit/core/version_test.cpp
    unit/core/product_test.cpp
    unit/core/filesystem_test.cpp
    unit/core/model/dependency_test.cpp
    unit/core/model/document_test.cpp
    unit/core/model/element_type_test.cpp
    unit/core/model/mesh_test.cpp
    unit/core/model/opaque_test.cpp
    unit/core/model/units_test.cpp)
target_link_libraries(fem_core_tests PRIVATE fem::core GTest::gtest_main)
target_compile_definitions(fem_core_tests PRIVATE FEM_EXPECTED_VERSION="${PROJECT_VERSION}"
                                                  FEM_EXPECTED_PRODUCT_NAME="${PROJECT_NAME}")
gtest_discover_tests(fem_core_tests DISCOVERY_MODE PRE_TEST)

# Model IR benchmark (task 1.2): one million nodes without copying the arrays. Fails if they are
# copied, so it runs with the tests; its timings are printed with `ctest -L benchmark -V`.
add_executable(fem_model_benchmark benchmark/mesh_benchmark.cpp)
target_link_libraries(fem_model_benchmark PRIVATE fem::core)
add_test(NAME ModelBenchmark.MillionNodeMesh COMMAND fem_model_benchmark)
set_tests_properties(ModelBenchmark.MillionNodeMesh PROPERTIES LABELS benchmark)

if(FEM_BUILD_GUI)
    add_executable(fem_shell_tests unit/shell/toolkit_info_test.cpp)
    target_link_libraries(fem_shell_tests PRIVATE fem::shell GTest::gtest_main)
    gtest_discover_tests(fem_shell_tests DISCOVERY_MODE PRE_TEST)
endif()
