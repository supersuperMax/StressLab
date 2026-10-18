# Built-in extensions, one folder per extension with its stresslab-extension.toml: fem.demo (task
# 1.15 of docs/plan.md), fem.model.structural, the types of a structural model and its results
# (task 3.1), and fem.format.abaqus-inp, the import of Abaqus input files (task 3.2).
#
# The application reads them from a copy in the build tree (FEM_STAGED_EXTENSIONS_DIR), laid out
# as the installed application will have them (task 3.11): the files a module needs at run time,
# and the libraries of native modules built into their folder. The sources (src/), tests and this
# file are not copied. Edits reach the copy with the next build.
set(FEM_STAGED_EXTENSIONS_DIR "${PROJECT_BINARY_DIR}/builtin-extensions")
set(FEM_STAGED_EXTENSIONS_DIR "${FEM_STAGED_EXTENSIONS_DIR}" PARENT_SCOPE)

set(fem_stage_outputs)
file(
    GLOB fem_extension_folders
    LIST_DIRECTORIES true
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(folder IN LISTS fem_extension_folders)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${folder}/stresslab-extension.toml")
        continue()
    endif()
    file(
        GLOB_RECURSE files
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/${folder}"
        CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${folder}/*")
    foreach(file IN LISTS files)
        if(file MATCHES "^(src|tests)/" OR file STREQUAL "CMakeLists.txt" OR file MATCHES
                                                                              "__pycache__|\\.pyc$")
            continue()
        endif()
        set(output "${FEM_STAGED_EXTENSIONS_DIR}/${folder}/${file}")
        add_custom_command(
            OUTPUT "${output}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${folder}/${file}"
                    "${output}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${folder}/${file}"
            VERBATIM)
        list(APPEND fem_stage_outputs "${output}")
    endforeach()
endforeach()
add_custom_target(fem_builtin_extensions ALL DEPENDS ${fem_stage_outputs})

add_subdirectory(fem.format.abaqus-inp)
add_dependencies(fem_builtin_extensions fem_format_abaqus_inp)
add_dependencies(stresslab fem_builtin_extensions)
