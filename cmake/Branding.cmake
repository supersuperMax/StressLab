# Paths to the brand kit for the shell (task 1.11) and for installers and packages (task 3.11).
# resources/branding/ is the source of truth: targets take the files from there as they are and
# never keep redrawn or recoloured copies (resources/branding/stresslab-kit/guidelines.md, section 7).

set(FEM_BRANDING_DIR "${PROJECT_SOURCE_DIR}/resources/branding/stresslab-kit")

# Application icon per platform.
# Windows: icon resource of the executable and the installer (16-256 px in one file).
set(FEM_BRANDING_ICON_WINDOWS "${FEM_BRANDING_DIR}/icons/windows/StressLab.ico")
# macOS: copied into Contents/Resources and named by CFBundleIconFile in Info.plist.
set(FEM_BRANDING_ICON_MACOS "${FEM_BRANDING_DIR}/icons/macos/StressLab.icns")
# Linux: the tree is installed into share/icons/hicolor/ (<size>/apps/stresslab.png, scalable,
# symbolic), the desktop entry into share/applications/; it refers to the icon as Icon=stresslab.
set(FEM_BRANDING_ICON_LINUX_HICOLOR_DIR "${FEM_BRANDING_DIR}/icons/linux/hicolor")
set(FEM_BRANDING_DESKTOP_FILE "${FEM_BRANDING_DIR}/icons/linux/stresslab.desktop")
# The application icon as a drawing: the window icon above 32 px on every OS.
set(FEM_BRANDING_ICON_SVG "${FEM_BRANDING_ICON_LINUX_HICOLOR_DIR}/scalable/apps/stresslab.svg")

# Logo for the About dialog: "color" on a light theme, "reversed" on a dark one.
set(FEM_BRANDING_LOGO_COLOR "${FEM_BRANDING_DIR}/logo/svg/stresslab-horizontal-color.svg")
set(FEM_BRANDING_LOGO_REVERSED "${FEM_BRANDING_DIR}/logo/svg/stresslab-horizontal-reversed.svg")
# Symbol drawn for 32 px and smaller: wider gaps and hole.
set(FEM_BRANDING_SYMBOL_SMALL "${FEM_BRANDING_DIR}/logo/svg/stresslab-symbol-small-color.svg")

# A moved or renamed kit file fails the configure step, not a packaging run much later.
set(_fem_branding_files
    "${FEM_BRANDING_ICON_WINDOWS}"
    "${FEM_BRANDING_ICON_MACOS}"
    "${FEM_BRANDING_ICON_SVG}"
    "${FEM_BRANDING_DESKTOP_FILE}"
    "${FEM_BRANDING_LOGO_COLOR}"
    "${FEM_BRANDING_LOGO_REVERSED}"
    "${FEM_BRANDING_SYMBOL_SMALL}")
foreach(path IN LISTS _fem_branding_files)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Brand kit file is missing: ${path}")
    endif()
endforeach()
unset(_fem_branding_files)
