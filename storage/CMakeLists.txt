# Project folder (.slproj), deterministic JSON, blob store and history (tasks 2.1-2.3 of
# docs/plan.md). Must build without Qt GUI and VTK (CLAUDE.md, rule 2).
add_library(femstorage)
add_library(fem::storage ALIAS femstorage)

target_sources(
    femstorage
    PRIVATE src/blob_store.cpp
            src/chunking.cpp
            src/error.cpp
            src/garbage.cpp
            src/history.cpp
            src/json.cpp
            src/names.cpp
            src/object_data_codec.cpp
            src/project.cpp
            src/project_format.cpp
            src/repository.cpp
            src/session.cpp
            src/sha256.cpp
            src/zstd_codec.cpp)
target_include_directories(femstorage PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(femstorage PUBLIC cxx_std_20)
target_link_libraries(femstorage PUBLIC fem::core)

# zstd compresses the blocks on disk (task 2.2); it is an implementation detail of the store.
find_package(zstd CONFIG REQUIRED)
target_link_libraries(femstorage PRIVATE zstd::libzstd)

# libgit2 keeps the history of the model in .fem/git (task 2.3); like zstd, it is not seen in the
# public headers.
find_package(libgit2 CONFIG REQUIRED)
target_link_libraries(femstorage PRIVATE libgit2::libgit2package)
