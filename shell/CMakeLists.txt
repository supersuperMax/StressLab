# Qt shell (task 1.11, ADR 0012): the window with its zones, themes, workspaces and the brand.
# What fills the zones comes from the registry of contributions (task 1.12, ADR 0013).
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)
find_package(qtadvanceddocking-qt6 CONFIG REQUIRED)

add_library(femshell)
add_library(fem::shell ALIAS femshell)

target_sources(
    femshell
    PRIVATE src/about_dialog.cpp
            src/brand.cpp
            src/command_palette.cpp
            src/commands.cpp
            src/contribution_ui.cpp
            src/icons.cpp
            src/keys.cpp
            src/main_window.cpp
            src/menu_order.cpp
            src/problems.cpp
            src/stages.cpp
            src/strings.cpp
            src/theme.cpp
            src/toolkit_info.cpp
            src/workspaces.cpp
            include/fem/shell/about_dialog.h
            include/fem/shell/command_palette.h
            include/fem/shell/contribution_ui.h
            include/fem/shell/main_window.h
            include/fem/shell/problems.h)
set_target_properties(femshell PROPERTIES AUTOMOC ON)
target_include_directories(femshell PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(femshell PUBLIC fem::core Qt6::Widgets Qt6::Svg ads::qtadvanceddocking-qt6)
# The core has its own `Signal::emit` and `slots_`: the shell and what uses it write Q_SIGNALS,
# Q_SLOTS and Q_EMIT instead of the keywords of Qt.
target_compile_definitions(femshell PUBLIC QT_NO_KEYWORDS)

# The brand kit goes in as it is, from the paths of cmake/Branding.cmake (no copies in shell/).
set(fem_brand_files
    "${FEM_BRANDING_SYMBOL_SMALL}" symbol-small-color.svg
    "${FEM_BRANDING_ICON_SVG}" app-icon.svg
    "${FEM_BRANDING_LOGO_COLOR}" logo-color.svg
    "${FEM_BRANDING_LOGO_REVERSED}" logo-reversed.svg)
set(fem_brand_sources "")
while(fem_brand_files)
    list(POP_FRONT fem_brand_files file alias)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}")
    list(APPEND fem_brand_sources "${file}")
endwhile()
qt_add_resources(femshell "stresslab_brand" PREFIX "/stresslab/brand" FILES ${fem_brand_sources})

# Strings of the shell (owner "fem.shell"), as catalogs of the localization of the core.
qt_add_resources(femshell "stresslab_shell_l10n" PREFIX "/stresslab/l10n" BASE l10n
                 FILES l10n/en.json l10n/ru.json)

# Where an executable with the window finds the plugins of Qt (platforms, styles). The shared Qt
# of vcpkg on Windows finds them relative to Qt6Core.dll, but fem_copy_runtime_dlls puts a copy
# of the DLLs next to the executable, away from the plugins: a qt.conf there names their folder.
# Static Qt (Linux and macOS in vcpkg) links the plugins in.
function(fem_write_qt_conf target)
    if(WIN32)
        file(GENERATE OUTPUT "$<TARGET_FILE_DIR:${target}>/qt.conf"
             CONTENT "[Paths]\nPlugins = $<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>/..\n")
    endif()
endfunction()
