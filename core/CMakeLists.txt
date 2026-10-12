# libfemcore: platform core. Must build without Qt GUI and VTK (CLAUDE.md, rule 2).
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

add_library(femcore)
add_library(fem::core ALIAS femcore)

target_sources(
    femcore
    PRIVATE src/version.cpp
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
            src/extension/manager.cpp
            src/extension/manifest.cpp
            src/extension/native_runtime.cpp
            src/extension/profile.cpp
            src/extension/registry.cpp
            src/extension/semver.cpp
            src/extension/shared_library.cpp
            src/model/change_set.cpp
            src/model/document.cpp
            src/model/document_editor.cpp
            src/model/element_type.cpp
            src/model/error.cpp
            src/model/mesh.cpp
            src/model/object.cpp
            src/model/units.cpp
            src/model/utf8.cpp
            src/text/number.cpp)
target_include_directories(femcore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(femcore PUBLIC cxx_std_20)
target_compile_definitions(femcore PRIVATE FEM_VERSION_STRING="${PROJECT_VERSION}")
# Object properties are nlohmann::json values, so the dependency is part of the public API.
target_link_libraries(femcore PUBLIC nlohmann_json::nlohmann_json)
# Manifests of modules are read with toml++ only inside the core; native modules are loaded with
# the dynamic loader (libdl where it is a separate library).
target_link_libraries(femcore PRIVATE tomlplusplus::tomlplusplus ${CMAKE_DL_LIBS})
