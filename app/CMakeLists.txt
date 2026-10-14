# `stresslab` executable. Task 1.8: the embedded Python (`stresslab --python ...`); task 1.10:
# `stresslab --headless run-script` (ADR 0011); task 1.11: the window (ADR 0012), in builds with
# the GUI.
#
# Everything but `main` is a library, so that the tests reach it.
add_library(stresslab_app STATIC src/application.cpp src/command_line.cpp
                                 src/executable_path.cpp src/headless.cpp src/module_pump.cpp)
target_include_directories(stresslab_app PUBLIC src)
# The core comes with the embedded interpreter.
target_link_libraries(stresslab_app PUBLIC fem_python_embed)

add_executable(stresslab main.cpp)
target_link_libraries(stresslab PRIVATE stresslab_app)
# The modules shipped with the application; from the source tree until the application is
# packaged (task 3.11), like the pure Python part of `stresslab`.
target_compile_definitions(stresslab PRIVATE
                           FEM_BUILTIN_EXTENSIONS_DIR="${PROJECT_SOURCE_DIR}/extensions")

# The icon of the executable on Windows, from the brand kit.
if(WIN32)
    configure_file(stresslab.rc.in stresslab.rc @ONLY)
    target_sources(stresslab PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/stresslab.rc)
endif()

if(FEM_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    add_library(stresslab_window STATIC src/console_engine.cpp src/log_slot.cpp src/window.cpp)
    target_link_libraries(stresslab_window PUBLIC stresslab_app fem::shell)
    target_link_libraries(stresslab PRIVATE stresslab_window)
    target_compile_definitions(stresslab PRIVATE FEM_WITH_WINDOW)
    fem_write_qt_conf(stresslab)

    if(APPLE)
        # An application bundle: the icon of the Dock and Finder is StressLab.icns from the kit,
        # named by CFBundleIconFile.
        get_filename_component(fem_icon_name "${FEM_BRANDING_ICON_MACOS}" NAME)
        target_sources(stresslab PRIVATE "${FEM_BRANDING_ICON_MACOS}")
        set_source_files_properties("${FEM_BRANDING_ICON_MACOS}" PROPERTIES
                                    MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(
            stresslab
            PROPERTIES MACOSX_BUNDLE ON
                       MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
                       MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}"
                       MACOSX_BUNDLE_EXECUTABLE_NAME stresslab
                       MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.supersupermax.stresslab"
                       MACOSX_BUNDLE_ICON_FILE "${fem_icon_name}"
                       MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
                       MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    elseif(UNIX)
        # Linux desktops find the icon by name in the hicolor theme and the application by its
        # desktop entry (Icon=stresslab). Installing a working application (Python, modules) is
        # task 3.11; these rules put the executable, the icons and the entry in place.
        include(GNUInstallDirs)
        install(TARGETS stresslab RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(DIRECTORY "${FEM_BRANDING_ICON_LINUX_HICOLOR_DIR}/"
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor)
        install(FILES "${FEM_BRANDING_DESKTOP_FILE}"
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    endif()
endif()

fem_copy_runtime_dlls(stresslab)
