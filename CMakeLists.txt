cmake_minimum_required(VERSION 3.28)

# On Windows the vcpkg triplets are built with MSVC. Ninja would otherwise pick up any MinGW
# compiler found first in PATH and fail at link time, so default to cl unless told otherwise.
if(CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER cl)
endif()
if(CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER cl)
endif()

# C is for native modules: the C ABI header of modules is checked by a test module in plain C.
project(
    StressLab
    VERSION 0.1.0
    DESCRIPTION "Cross-platform finite element analysis environment"
    LANGUAGES C CXX)

option(FEM_BUILD_GUI "Build the Qt shell and other GUI components" ON)
option(FEM_BUILD_TESTS "Build unit tests" ON)

# Brand kit paths (icons, logos) for the shell and packaging.
include(cmake/Branding.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W4 $<$<COMPILE_LANGUAGE:CXX>:/permissive->
                        $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(core)
add_subdirectory(storage)
add_subdirectory(python)
add_subdirectory(app)
add_subdirectory(extensions)
if(FEM_BUILD_GUI)
    add_subdirectory(shell)
endif()

if(FEM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
