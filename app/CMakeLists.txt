# `stresslab` executable. Task 1.8: the embedded Python (`stresslab --python ...`); task 1.10:
# `stresslab --headless run-script` (ADR 0011); the GUI comes with 1.11.
#
# Everything but `main` is a library, so that the tests reach it.
add_library(stresslab_app STATIC src/command_line.cpp src/executable_path.cpp src/headless.cpp
                                 src/module_pump.cpp)
target_include_directories(stresslab_app PUBLIC src)
# The core comes with the embedded interpreter.
target_link_libraries(stresslab_app PUBLIC fem_python_embed)

add_executable(stresslab main.cpp)
target_link_libraries(stresslab PRIVATE stresslab_app)
# The modules shipped with the application; from the source tree until the application is
# packaged (task 3.11), like the pure Python part of `stresslab`.
target_compile_definitions(stresslab PRIVATE
                           FEM_BUILTIN_EXTENSIONS_DIR="${PROJECT_SOURCE_DIR}/extensions")
fem_copy_runtime_dlls(stresslab)
