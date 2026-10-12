# libfemcore: platform core. Must build without Qt GUI and VTK (CLAUDE.md, rule 2).
find_package(nlohmann_json CONFIG REQUIRED)

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
            src/model/change_set.cpp
            src/model/document.cpp
            src/model/document_editor.cpp
            src/model/element_type.cpp
            src/model/error.cpp
            src/model/mesh.cpp
            src/model/object.cpp
            src/model/units.cpp
            src/model/utf8.cpp)
target_include_directories(femcore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(femcore PUBLIC cxx_std_20)
target_compile_definitions(femcore PRIVATE FEM_VERSION_STRING="${PROJECT_VERSION}")
# Object properties are nlohmann::json values, so the dependency is part of the public API.
target_link_libraries(femcore PUBLIC nlohmann_json::nlohmann_json)
