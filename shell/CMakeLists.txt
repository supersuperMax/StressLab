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
            src/comparison_view.cpp
            src/contribution_ui.cpp
            src/extensions_panel.cpp
            src/file_dialogs.cpp
            src/form_widget.cpp
            src/history_controller.cpp
            src/history_graph.cpp
            src/history_panel.cpp
            src/history_text.cpp
            src/icons.cpp
            src/journal_panel.cpp
            src/keys.cpp
            src/main_window.cpp
            src/menu_order.cpp
            src/model_tree.cpp
            src/problems.cpp
            src/project_actions.cpp
            src/property_panel.cpp
            src/python_console.cpp
            src/required_modules_bar.cpp
            src/settings_panel.cpp
            src/stages.cpp
            src/strings.cpp
            src/theme.cpp
            src/toolkit_info.cpp
            src/version_bar.cpp
            src/workspaces.cpp
            include/fem/shell/about_dialog.h
            include/fem/shell/command_palette.h
            include/fem/shell/comparison_view.h
            include/fem/shell/contribution_ui.h
            include/fem/shell/extensions_panel.h
            include/fem/shell/form_widget.h
            include/fem/shell/history_controller.h
            include/fem/shell/history_panel.h
            include/fem/shell/journal_panel.h
            include/fem/shell/main_window.h
            include/fem/shell/model_tree.h
            include/fem/shell/problems.h
            include/fem/shell/project_actions.h
            include/fem/shell/property_panel.h
            include/fem/shell/python_console.h
            include/fem/shell/required_modules_bar.h
            include/fem/shell/settings_panel.h
            include/fem/shell/version_bar.h)
set_target_properties(femshell PROPERTIES AUTOMOC ON)
target_include_directories(femshell PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(femshell PUBLIC fem::core fem::storage Qt6::Widgets Qt6::Svg ads::qtadvanceddocking-qt6)
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

# Icons of the activities of the shell: monochrome, tinted to the theme like icons of modules.
qt_add_resources(femshell "stresslab_shell_icons" PREFIX "/stresslab/icons" BASE icons
                 FILES icons/extensions.svg icons/history.svg icons/model.svg icons/settings.svg)

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
