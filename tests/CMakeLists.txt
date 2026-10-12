find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(
    fem_core_tests
    unit/core/version_test.cpp
    unit/core/product_test.cpp
    unit/core/concurrent_signal_test.cpp
    unit/core/filesystem_test.cpp
    unit/core/signal_test.cpp
    unit/core/command/command_bus_test.cpp
    unit/core/command/journal_test.cpp
    unit/core/command/model_commands_test.cpp
    unit/core/command/python_literal_test.cpp
    unit/core/command/transaction_test.cpp
    unit/core/command/undo_test.cpp
    unit/core/context/context_keys_test.cpp
    unit/core/context/core_keys_test.cpp
    unit/core/context/watch_test.cpp
    unit/core/context/when_evaluate_test.cpp
    unit/core/context/when_grammar_test.cpp
    unit/core/extension/activation_test.cpp
    unit/core/extension/manager_test.cpp
    unit/core/extension/manifest_test.cpp
    unit/core/extension/native_test.cpp
    unit/core/extension/profile_test.cpp
    unit/core/extension/registry_test.cpp
    unit/core/extension/semver_test.cpp
    unit/core/job/job_manager_test.cpp
    unit/core/job/local_executor_test.cpp
    unit/core/model/dependency_test.cpp
    unit/core/model/document_test.cpp
    unit/core/model/element_type_test.cpp
    unit/core/model/mesh_test.cpp
    unit/core/model/opaque_test.cpp
    unit/core/model/units_test.cpp
    unit/core/task/task_pool_test.cpp
    unit/core/task/task_test.cpp)
target_link_libraries(fem_core_tests PRIVATE fem::core GTest::gtest_main)
target_compile_definitions(
    fem_core_tests
    PRIVATE FEM_EXPECTED_VERSION="${PROJECT_VERSION}"
            FEM_EXPECTED_PRODUCT_NAME="${PROJECT_NAME}"
            FEM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
            FEM_TEST_EXTENSIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/extensions"
            FEM_TEST_NATIVE_HELLO="$<TARGET_FILE:fem_test_extension_hello>"
            FEM_TEST_NATIVE_BAD_ABI="$<TARGET_FILE:fem_test_extension_hello_bad_abi>"
            FEM_TEST_FAKE_PROCESS="$<TARGET_FILE:fem_test_fake_process>")
gtest_discover_tests(fem_core_tests DISCOVERY_MODE PRE_TEST)

# Native test modules (task 1.5), built like modules made outside the platform: plain C, only the
# C ABI header, nothing of the core linked. `hello_bad_abi` claims an ABI version the platform does
# not support. The file name is what `entry` names plus the suffix the platform looks for.
foreach(fixture IN ITEMS hello hello_bad_abi)
    add_library(fem_test_extension_${fixture} MODULE extensions/native-hello/hello.c)
    target_include_directories(fem_test_extension_${fixture} PRIVATE ${PROJECT_SOURCE_DIR}/core/include)
    set_target_properties(
        fem_test_extension_${fixture}
        PROPERTIES PREFIX ""
                   OUTPUT_NAME ${fixture}
                   C_VISIBILITY_PRESET hidden
                   LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/extension-fixtures)
    if(APPLE)
        set_target_properties(fem_test_extension_${fixture} PROPERTIES SUFFIX ".dylib")
    endif()
    add_dependencies(fem_core_tests fem_test_extension_${fixture})
endforeach()
target_compile_definitions(fem_test_extension_hello_bad_abi PRIVATE HELLO_ABI_VERSION=999)

# The fake solver of the local executor tests (task 1.6). The tests copy it into a folder with
# Cyrillic letters and spaces, so it depends on nothing: the C runtime is linked statically.
add_executable(fem_test_fake_process fixtures/fake-process/fake_process.cpp)
set_target_properties(fem_test_fake_process PROPERTIES MSVC_RUNTIME_LIBRARY
                                                       "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(fem_core_tests fem_test_fake_process)

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
