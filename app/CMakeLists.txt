# `stresslab` executable. Task 1.8: the embedded Python (`stresslab --python ...`);
# `--headless run-script` comes with task 1.10, the GUI with 1.11.
add_executable(stresslab main.cpp)
target_link_libraries(stresslab PRIVATE fem::core fem_python_embed)
fem_copy_runtime_dlls(stresslab)
