# Python API (task 1.8, docs/architecture.md §4.7): the `stresslab` package with its native part
# `_stresslab` (nanobind), and the embedded interpreter of the application. The extension host
# (task 1.9) comes later.
#
# Python is found by the root CMakeLists.txt. nanobind comes from pip
# (python/requirements-dev.txt), not from vcpkg: the vcpkg port builds a Python of its own
# (ADR 0009).
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_VARIABLE fem_nanobind_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE fem_nanobind_result)
if(NOT fem_nanobind_result EQUAL 0)
    message(FATAL_ERROR "nanobind is not installed for ${Python_EXECUTABLE}; "
                        "run: python -m pip install -r python/requirements-dev.txt")
endif()
file(TO_CMAKE_PATH "${fem_nanobind_dir}" fem_nanobind_dir)
find_package(nanobind CONFIG REQUIRED PATHS "${fem_nanobind_dir}" NO_DEFAULT_PATH)

# nanobind's own sources are third-party code: built without our warning level.
nanobind_build_library(nanobind-static AS_SYSINCLUDE)
get_target_property(fem_nanobind_options nanobind-static COMPILE_OPTIONS)
list(REMOVE_ITEM fem_nanobind_options /W4 -Wall -Wextra -Wpedantic)
set_target_properties(nanobind-static PROPERTIES COMPILE_OPTIONS "${fem_nanobind_options}")

# Where the build puts the importable package: the native module, the pure Python part and, on
# Windows, the DLLs the module needs. Tests and the embedded interpreter import from here.
set(FEM_PYTHON_SITE_DIR "${CMAKE_CURRENT_BINARY_DIR}/site")
set(FEM_PYTHON_SITE_DIR "${FEM_PYTHON_SITE_DIR}" PARENT_SCOPE)

# --- Bindings, shared by the extension module and the embedded interpreter ----------------------

add_library(fem_python_bindings STATIC
    bindings/src/context.cpp
    bindings/src/convert.cpp
    bindings/src/documents.cpp
    bindings/src/errors.cpp
    bindings/src/mesh.cpp
    bindings/src/module.cpp
    bindings/src/session.cpp
    bindings/src/tasks.cpp)
target_include_directories(fem_python_bindings PUBLIC bindings/include)
target_link_libraries(fem_python_bindings PUBLIC fem::core nanobind-static)
nanobind_compile_options(fem_python_bindings)
nanobind_set_visibility(fem_python_bindings)

# --- The extension module for plain Python ------------------------------------------------------

# What nanobind_add_module(NB_STATIC) does, except linking nanobind a second time: it comes with
# the bindings, and a library repeated on the link line makes the macOS linker warn.
add_library(fem_python_module MODULE bindings/module.cpp)
target_link_libraries(fem_python_module PRIVATE fem_python_bindings)
set_target_properties(fem_python_module PROPERTIES LINKER_LANGUAGE CXX)
nanobind_compile_options(fem_python_module)
nanobind_link_options(fem_python_module)
nanobind_extension(fem_python_module)
nanobind_disable_stack_protector(fem_python_module)
nanobind_disable_stack_protector(nanobind-static)
nanobind_opt_size(fem_python_module)
nanobind_strip(fem_python_module)
nanobind_set_visibility(fem_python_module)
set_target_properties(fem_python_module PROPERTIES OUTPUT_NAME _stresslab
                                                   LIBRARY_OUTPUT_DIRECTORY "${FEM_PYTHON_SITE_DIR}")
if(WIN32)
    # Python looks for the DLLs of an extension module next to it, not on PATH. The Python DLL
    # itself is loaded already by the interpreter that imports the module.
    add_custom_command(
        TARGET fem_python_module POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "$<FILTER:$<TARGET_RUNTIME_DLLS:fem_python_module>,EXCLUDE,[/\]python[0-9]*\.dll$>"
            "${FEM_PYTHON_SITE_DIR}"
        COMMAND_EXPAND_LISTS)
endif()

# --- The pure Python part -----------------------------------------------------------------------

set(fem_python_package_files stresslab/__init__.py stresslab/_commands.py stresslab/py.typed)
set(fem_python_package_outputs)
foreach(file IN LISTS fem_python_package_files)
    add_custom_command(
        OUTPUT "${FEM_PYTHON_SITE_DIR}/${file}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${file}"
                "${FEM_PYTHON_SITE_DIR}/${file}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${file}"
        COMMENT "Copying ${file}")
    list(APPEND fem_python_package_outputs "${FEM_PYTHON_SITE_DIR}/${file}")
endforeach()
add_custom_target(fem_python_package ALL DEPENDS ${fem_python_package_outputs})
add_dependencies(fem_python_module fem_python_package)

# --- The embedded interpreter -------------------------------------------------------------------

# The installation the embedded interpreter takes the standard library from (its base prefix) and
# the site-packages of the Python found above, which may be a virtual environment.
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c
            "import sys, sysconfig; print(sys.base_prefix); print(sysconfig.get_path('purelib')); print(sysconfig.get_path('platlib'))"
    OUTPUT_VARIABLE fem_python_layout
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\n" ";" fem_python_layout "${fem_python_layout}")
list(GET fem_python_layout 0 fem_python_home)
list(SUBLIST fem_python_layout 1 2 fem_python_site_packages)
list(REMOVE_DUPLICATES fem_python_site_packages)
file(TO_CMAKE_PATH "${fem_python_home}" fem_python_home)
file(TO_CMAKE_PATH "${fem_python_site_packages}" fem_python_site_packages)
if(WIN32)
    set(fem_path_separator ";")
else()
    set(fem_path_separator ":")
endif()
list(JOIN fem_python_site_packages "${fem_path_separator}" fem_python_site_packages)

# The native module is built in; its pure Python part is imported from the site folder.
add_library(fem_python_embed STATIC embed/src/interpreter.cpp bindings/module.cpp)
target_include_directories(fem_python_embed PUBLIC embed/include)
target_link_libraries(fem_python_embed PUBLIC fem_python_bindings Python::Python)
target_compile_definitions(
    fem_python_embed
    PRIVATE FEM_PYTHON_SITE_DIR="${FEM_PYTHON_SITE_DIR}"
            FEM_PYTHON_HOME="${fem_python_home}"
            FEM_PYTHON_SITE_PACKAGES="${fem_python_site_packages}"
            FEM_PYTHON_EXECUTABLE="${Python_EXECUTABLE}")
nanobind_compile_options(fem_python_embed)
add_dependencies(fem_python_embed fem_python_package)

# Copies the DLLs an executable that embeds Python needs (python312.dll and those of the core)
# next to it, so it starts without Python on PATH.
function(fem_copy_runtime_dlls target)
    if(WIN32)
        add_custom_command(
            TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${target}>
                    $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()
