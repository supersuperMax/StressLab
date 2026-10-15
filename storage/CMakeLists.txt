# Project folder (.slproj), deterministic JSON, blob store and history (tasks 2.1-2.3 of
# docs/plan.md). Must build without Qt GUI and VTK (CLAUDE.md, rule 2).
add_library(femstorage)
add_library(fem::storage ALIAS femstorage)

target_sources(
    femstorage
    PRIVATE src/blob_store.cpp
            src/error.cpp
            src/json.cpp
            src/object_data_codec.cpp
            src/project.cpp
            src/project_format.cpp
            src/session.cpp
            src/sha256.cpp)
target_include_directories(femstorage PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(femstorage PUBLIC cxx_std_20)
target_link_libraries(femstorage PUBLIC fem::core)
