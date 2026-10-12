# Qt shell. The window itself is built in task 1.11 of docs/plan.md; for now this target only
# proves that the GUI toolchain (Qt 6 Widgets + Qt Advanced Docking System) resolves and compiles.
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(qtadvanceddocking-qt6 CONFIG REQUIRED)

add_library(femshell)
add_library(fem::shell ALIAS femshell)

target_sources(femshell PRIVATE src/toolkit_info.cpp)
target_include_directories(femshell PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(femshell PUBLIC fem::core Qt6::Widgets ads::qtadvanceddocking-qt6)
