# libfemcore: platform core. Must build without Qt GUI and VTK (CLAUDE.md, rule 2).
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(femcore)
add_library(fem::core ALIAS femcore)

# String catalogs of the core, built into the library (task 1.7).
include(${PROJECT_SOURCE_DIR}/cmake/EmbedCatalogs.cmake)
fem_embed_catalogs(${CMAKE_CURRENT_BINARY_DIR}/generated/core_catalogs.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/l10n/en.json ${CMAKE_CURRENT_SOURCE_DIR}/l10n/ru.json)

target_sources(
    femcore
    PRIVATE src/bindings.cpp
            src/user_folders.cpp
            src/version.cpp
            src/command/command.cpp
            src/command/command_bus.cpp
            src/command/error.cpp
            src/command/journal.cpp
            src/command/model_commands.cpp
            src/command/python_literal.cpp
            src/command/value.cpp
            src/context/context_keys.cpp
            src/context/core_keys.cpp
            src/context/error.cpp
            src/context/value.cpp
            src/context/when.cpp
            src/context/when_parser.cpp
            src/extension/contributions.cpp
            src/extension/error.cpp
            src/extension/files.cpp
            src/extension/host_data.cpp
            src/extension/host_runtime.cpp
            src/extension/json_value.cpp
            src/extension/manager.cpp
            src/extension/manifest.cpp
            src/extension/native_runtime.cpp
            src/extension/profile.cpp
            src/extension/registry.cpp
            src/extension/semver.cpp
            src/extension/shared_library.cpp
            src/job/error.cpp
            src/job/job_keys.cpp
            src/job/job_manager.cpp
            src/job/job_spec.cpp
            src/job/local_executor.cpp
            src/io/files.cpp
            src/ipc/json_rpc.cpp
            src/ipc/shared_memory.cpp
            src/l10n/catalog.cpp
            src/l10n/error.cpp
            src/l10n/language.cpp
            src/l10n/localization.cpp
            src/log/log.cpp
            src/log/rotating_file_sink.cpp
            src/model/change_set.cpp
            src/model/document.cpp
            src/model/document_editor.cpp
            src/model/element_type.cpp
            src/model/error.cpp
            src/model/mesh.cpp
            src/model/object.cpp
            src/model/units.cpp
            src/model/utf8.cpp
            src/settings/error.cpp
            src/settings/settings.cpp
            src/settings/validate.cpp
            src/task/error.cpp
            src/task/task.cpp
            src/task/task_pool.cpp
            src/text/number.cpp
            ${CMAKE_CURRENT_BINARY_DIR}/generated/core_catalogs.cpp)
# Child processes of the local executor: a Job Object on Windows, a process group elsewhere.
if(WIN32)
    target_sources(femcore PRIVATE src/job/process_win32.cpp)
    # Known folders of the user (user_folders.cpp).
    target_link_libraries(femcore PRIVATE shell32 ole32)
else()
    target_sources(femcore PRIVATE src/job/process_posix.cpp)
endif()
# shm_open is in librt on glibc older than 2.34; the library is still there (empty) on newer ones.
if(UNIX AND NOT APPLE)
    target_link_libraries(femcore PRIVATE rt)
endif()
target_include_directories(femcore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# The generated catalog source includes its header from src/.
target_include_directories(femcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(femcore PUBLIC cxx_std_20)
target_compile_definitions(femcore PRIVATE FEM_VERSION_STRING="${PROJECT_VERSION}")
# Object properties are nlohmann::json values, so the dependency is part of the public API.
# Tasks and jobs run on threads of their own. Loggers are spdlog loggers (with {fmt}).
target_link_libraries(femcore PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog Threads::Threads)
# Manifests of modules are read with toml++ only inside the core; native modules are loaded with
# the dynamic loader (libdl where it is a separate library).
target_link_libraries(femcore PRIVATE tomlplusplus::tomlplusplus ${CMAKE_DL_LIBS})
