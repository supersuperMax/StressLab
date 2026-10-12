# libfemcore: platform core. Must build without Qt GUI and VTK (CLAUDE.md, rule 2).
add_library(femcore)
add_library(fem::core ALIAS femcore)

target_sources(femcore PRIVATE src/version.cpp)
target_include_directories(femcore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(femcore PUBLIC cxx_std_20)
target_compile_definitions(femcore PRIVATE FEM_VERSION_STRING="${PROJECT_VERSION}")
