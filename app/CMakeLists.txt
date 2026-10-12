# `stresslab` executable. Task 1.8: the embedded Python (`stresslab --python ...`);
# `--headless run-script` comes with task 1.10, the GUI with 1.11.
add_executable(stresslab main.cpp)
# The core comes with the embedded interpreter.
target_link_libraries(stresslab PRIVATE fem_python_embed)
fem_copy_runtime_dlls(stresslab)
