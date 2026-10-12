find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(fem_core_tests unit/core/version_test.cpp unit/core/filesystem_test.cpp)
target_link_libraries(fem_core_tests PRIVATE fem::core GTest::gtest_main)
target_compile_definitions(fem_core_tests PRIVATE FEM_EXPECTED_VERSION="${PROJECT_VERSION}")
gtest_discover_tests(fem_core_tests DISCOVERY_MODE PRE_TEST)

if(FEM_BUILD_GUI)
    add_executable(fem_shell_tests unit/shell/toolkit_info_test.cpp)
    target_link_libraries(fem_shell_tests PRIVATE fem::shell GTest::gtest_main)
    gtest_discover_tests(fem_shell_tests DISCOVERY_MODE PRE_TEST)
endif()
