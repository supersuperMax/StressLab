# fem.format.abaqus-inp (tasks 3.2 and 3.3, docs/adr/0024-abaqus-inp-import.md,
# docs/adr/0025-abaqus-inp-export.md): a native module. The reader and writer are a static library without the core, so that the tests use it directly; the module
# links it with the C ABI glue and depends on nothing of the platform but fem/extension_abi.h.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(
    fem_inp_reader STATIC src/inp/deck.cpp src/inp/elements.cpp src/inp/encoding.cpp
                          src/inp/export.cpp src/inp/import.cpp src/inp/numbers.cpp)
target_include_directories(fem_inp_reader PUBLIC src)
target_link_libraries(fem_inp_reader PUBLIC nlohmann_json::nlohmann_json)

add_library(fem_format_abaqus_inp MODULE src/module.cpp)
target_include_directories(fem_format_abaqus_inp PRIVATE ${PROJECT_SOURCE_DIR}/core/include)
target_link_libraries(fem_format_abaqus_inp PRIVATE fem_inp_reader)
# The file name is what `entry` names plus the suffix the platform looks for, in the copy of the
# module folder the application reads (extensions/CMakeLists.txt).
set_target_properties(
    fem_format_abaqus_inp
    PROPERTIES PREFIX ""
               OUTPUT_NAME fem_format_abaqus_inp
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               LIBRARY_OUTPUT_DIRECTORY "${FEM_STAGED_EXTENSIONS_DIR}/fem.format.abaqus-inp"
               RUNTIME_OUTPUT_DIRECTORY "${FEM_STAGED_EXTENSIONS_DIR}/fem.format.abaqus-inp"
               ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
if(APPLE)
    set_target_properties(fem_format_abaqus_inp PROPERTIES SUFFIX ".dylib")
endif()
