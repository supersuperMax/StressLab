find_package(GTest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
include(GoogleTest)

add_executable(
    fem_core_tests
    unit/core/version_test.cpp
    unit/core/bindings_test.cpp
    unit/core/product_test.cpp
    unit/core/concurrent_signal_test.cpp
    unit/core/filesystem_test.cpp
    unit/core/signal_test.cpp
    unit/core/command/command_bus_test.cpp
    unit/core/command/journal_test.cpp
    unit/core/command/model_commands_test.cpp
    unit/core/command/set_property_test.cpp
    unit/core/command/python_literal_test.cpp
    unit/core/command/transaction_test.cpp
    unit/core/command/undo_test.cpp
    unit/core/context/context_keys_test.cpp
    unit/core/context/core_keys_test.cpp
    unit/core/context/watch_test.cpp
    unit/core/context/when_evaluate_test.cpp
    unit/core/context/when_grammar_test.cpp
    unit/core/context/when_unmet_test.cpp
    unit/core/extension/activation_test.cpp
    unit/core/extension/host_runtime_test.cpp
    unit/core/extension/manager_test.cpp
    unit/core/extension/manifest_test.cpp
    unit/core/extension/native_test.cpp
    unit/core/extension/profile_test.cpp
    unit/core/extension/registry_test.cpp
    unit/core/extension/semver_test.cpp
    unit/core/form/form_test.cpp
    unit/core/form/status_test.cpp
    unit/core/job/job_keys_test.cpp
    unit/core/job/job_manager_test.cpp
    unit/core/job/local_executor_test.cpp
    unit/core/ipc/json_rpc_test.cpp
    unit/core/ipc/shared_memory_test.cpp
    unit/core/l10n/catalog_test.cpp
    unit/core/l10n/core_catalogs_test.cpp
    unit/core/l10n/localization_test.cpp
    unit/core/log/log_test.cpp
    unit/core/model/dependency_test.cpp
    unit/core/model/document_test.cpp
    unit/core/model/element_type_test.cpp
    unit/core/model/mesh_test.cpp
    unit/core/model/opaque_test.cpp
    unit/core/model/quantity_test.cpp
    unit/core/model/units_test.cpp
    unit/core/settings/settings_test.cpp
    unit/core/settings/settings_validation_test.cpp
    unit/core/task/task_pool_test.cpp
    unit/core/task/task_test.cpp
    unit/core/user_folders_test.cpp)
target_link_libraries(fem_core_tests PRIVATE fem::core GTest::gtest_main)
target_compile_definitions(
    fem_core_tests
    PRIVATE FEM_EXPECTED_VERSION="${PROJECT_VERSION}"
            FEM_EXPECTED_PRODUCT_NAME="${PROJECT_NAME}"
            FEM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
            FEM_CORE_L10N_DIR="${PROJECT_SOURCE_DIR}/core/l10n"
            FEM_TEST_EXTENSIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/extensions"
            FEM_TEST_NATIVE_HELLO="$<TARGET_FILE:fem_test_extension_hello>"
            FEM_TEST_NATIVE_BAD_ABI="$<TARGET_FILE:fem_test_extension_hello_bad_abi>"
            FEM_TEST_FAKE_PROCESS="$<TARGET_FILE:fem_test_fake_process>"
            FEM_TEST_PROCESS_MODULE="$<TARGET_FILE:fem_test_process_module>"
            FEM_TEST_PYTHON="${Python_EXECUTABLE}"
            FEM_TEST_HOST_PACKAGE_DIR="${PROJECT_SOURCE_DIR}/python"
            FEM_TEST_STRESSLAB="$<TARGET_FILE:stresslab>")
gtest_discover_tests(fem_core_tests DISCOVERY_MODE PRE_TEST)

# The Python API (task 1.8): journals run as Python scripts by the embedded interpreter, and the
# pytest suite run both by the embedded interpreter and by plain Python.
add_executable(fem_python_tests unit/python/interpreter_test.cpp unit/python/journal_script_test.cpp
                                unit/python/python_test_support.cpp)
target_link_libraries(fem_python_tests PRIVATE fem_python_embed GTest::gtest_main)
target_compile_definitions(fem_python_tests PRIVATE FEM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
fem_copy_runtime_dlls(fem_python_tests)
gtest_discover_tests(fem_python_tests DISCOVERY_MODE PRE_TEST)

execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import pytest, numpy"
    RESULT_VARIABLE fem_pytest_missing
    OUTPUT_QUIET ERROR_QUIET)
if(fem_pytest_missing)
    message(WARNING "pytest or numpy is not installed for ${Python_EXECUTABLE}: the Python tests "
                    "are not added to ctest (python -m pip install -r python/requirements-dev.txt)")
else()
    set(fem_pytest_args -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/python/tests)
    add_test(NAME Python.Pytest.Embedded COMMAND stresslab --python ${fem_pytest_args}
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    add_test(NAME Python.Pytest.Plain COMMAND "${Python_EXECUTABLE}" ${fem_pytest_args}
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    # The headless tests (task 1.10) start the executable themselves, in the plain run.
    set_tests_properties(
        Python.Pytest.Plain
        PROPERTIES ENVIRONMENT
                   "STRESSLAB_PYTHON_SITE=${FEM_PYTHON_SITE_DIR};STRESSLAB_EXECUTABLE=$<TARGET_FILE:stresslab>")
endif()

# The executable (task 1.10): its command line. The headless runs themselves are tested by
# pytest (python/tests/test_headless.py) with the real executable.
add_executable(stresslab_app_tests unit/app/command_line_test.cpp)
target_link_libraries(stresslab_app_tests PRIVATE stresslab_app GTest::gtest_main)
# A folder of its own: fem_python_tests copies the same DLLs (python312.dll) next to itself after
# linking, and two copies of one file at once fail on Windows.
set_target_properties(stresslab_app_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                     ${CMAKE_CURRENT_BINARY_DIR}/app)
fem_copy_runtime_dlls(stresslab_app_tests)
gtest_discover_tests(stresslab_app_tests DISCOVERY_MODE PRE_TEST)

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

# The process module of the extension host tests (task 1.9): speaks the protocol of modules itself.
# Copied next to its manifest into a folder with Cyrillic letters, so the C runtime is static.
add_executable(fem_test_process_module fixtures/process-module/process_module.cpp)
target_link_libraries(fem_test_process_module PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(fem_test_process_module PROPERTIES MSVC_RUNTIME_LIBRARY
                                                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# The host tests also start the extension host on the interpreter of the application.
add_dependencies(fem_core_tests fem_test_process_module stresslab fem_python_package)

# Model IR benchmark (task 1.2): one million nodes without copying the arrays. Fails if they are
# copied, so it runs with the tests; its timings are printed with `ctest -L benchmark -V`.
add_executable(fem_model_benchmark benchmark/mesh_benchmark.cpp)
target_link_libraries(fem_model_benchmark PRIVATE fem::core)
add_test(NAME ModelBenchmark.MillionNodeMesh COMMAND fem_model_benchmark)
set_tests_properties(ModelBenchmark.MillionNodeMesh PROPERTIES LABELS benchmark)

if(FEM_BUILD_GUI)
    # The shell (tasks 1.11-1.13): the window, its zones, themes, workspaces, the brand, what
    # modules contribute to it, the model tree and the forms of properties and settings. The tests run on the platform of Qt without a screen
    # (shell_test_main.cpp) and use Qt Test for clicks and keys; with STRESSLAB_SCREENSHOT_DIR set
    # they also save pictures of the window there.
    find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
    add_executable(
        fem_shell_tests
        unit/shell/shell_test_main.cpp
        unit/shell/brand_test.cpp
        unit/shell/command_palette_test.cpp
        unit/shell/contribution_ui_test.cpp
        unit/shell/form_widget_test.cpp
        unit/shell/icons_test.cpp
        unit/shell/keys_test.cpp
        unit/shell/main_window_test.cpp
        unit/shell/model_tree_test.cpp
        unit/shell/order_test.cpp
        unit/shell/screenshot_test.cpp
        unit/shell/settings_panel_test.cpp
        unit/shell/strings_test.cpp
        unit/shell/theme_test.cpp
        unit/shell/toolkit_info_test.cpp
        unit/shell/workspaces_test.cpp)
    target_link_libraries(fem_shell_tests PRIVATE fem::shell Qt6::Test GTest::gtest)
    target_compile_definitions(
        fem_shell_tests
        PRIVATE FEM_SHELL_L10N_DIR="${PROJECT_SOURCE_DIR}/shell/l10n"
                FEM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                FEM_BRANDING_SYMBOL_SMALL="${FEM_BRANDING_SYMBOL_SMALL}"
                FEM_BRANDING_ICON_SVG="${FEM_BRANDING_ICON_SVG}"
                FEM_BRANDING_LOGO_COLOR="${FEM_BRANDING_LOGO_COLOR}"
                FEM_BRANDING_LOGO_REVERSED="${FEM_BRANDING_LOGO_REVERSED}")
    # A static Qt (Linux and macOS in vcpkg) links in only the plugins named.
    qt_import_plugins(fem_shell_tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    set_target_properties(fem_shell_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                     ${CMAKE_CURRENT_BINARY_DIR}/shell)
    fem_copy_runtime_dlls(fem_shell_tests)
    fem_write_qt_conf(fem_shell_tests)
    gtest_discover_tests(fem_shell_tests DISCOVERY_MODE PRE_TEST)

    # The window part of the executable: log files of several windows.
    add_executable(stresslab_window_tests unit/app/log_slot_test.cpp)
    target_link_libraries(stresslab_window_tests PRIVATE stresslab_window GTest::gtest_main)
    set_target_properties(stresslab_window_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                            ${CMAKE_CURRENT_BINARY_DIR}/window)
    fem_copy_runtime_dlls(stresslab_window_tests)
    gtest_discover_tests(stresslab_window_tests DISCOVERY_MODE PRE_TEST)
endif()
