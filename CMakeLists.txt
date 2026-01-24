cmake_minimum_required(VERSION 3.20)
project(vgeo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(VGEO_BUILD_VIEWER "Build standalone viewer" ON)
option(VGEO_BUILD_TOOLS "Build CLI tools" ON)
option(VGEO_BUILD_TESTS "Build tests" ON)

# Find dependencies
find_package(Vulkan REQUIRED)

# GLFW for windowing
include(FetchContent)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.3.8
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# Subdirectories
add_subdirectory(src/core)
add_subdirectory(src/preprocess)
add_subdirectory(src/runtime)

if(VGEO_BUILD_VIEWER)
    add_subdirectory(src/viewer)
endif()

if(VGEO_BUILD_TOOLS)
    add_subdirectory(tools/vgeo_build)
    add_subdirectory(tools/vgeo_validate)
endif()

if(VGEO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
