cmake_minimum_required(VERSION 3.20)
project(vgeo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(VGEO_BUILD_VIEWER "Build standalone viewer (requires Vulkan)" ON)
option(VGEO_BUILD_TOOLS "Build CLI tools" ON)
option(VGEO_BUILD_TESTS "Build tests" ON)
option(VGEO_BUILD_PYTHON "Build Python bindings for Blender" ON)

# Find Vulkan (optional - only needed for viewer/runtime)
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "Vulkan found: ${Vulkan_INCLUDE_DIRS}")
    set(VGEO_HAS_VULKAN ON)
else()
    message(WARNING "Vulkan SDK not found. Viewer and runtime will not be built.")
    message(WARNING "Install Vulkan SDK from https://vulkan.lunarg.com/ to enable rendering.")
    set(VGEO_HAS_VULKAN OFF)
    set(VGEO_BUILD_VIEWER OFF)
    # Python bindings wrap the Vulkan runtime, so they cannot build without it
    set(VGEO_BUILD_PYTHON OFF)
endif()

# GLFW for windowing (only if building viewer)
if(VGEO_HAS_VULKAN AND VGEO_BUILD_VIEWER)
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
endif()

# Subdirectories
add_subdirectory(src/core)
add_subdirectory(src/preprocess)
add_subdirectory(src/scene)

# Runtime and viewer require Vulkan
if(VGEO_HAS_VULKAN)
    add_subdirectory(src/runtime)
    if(VGEO_BUILD_VIEWER)
        add_subdirectory(src/viewer)
    endif()
endif()

if(VGEO_BUILD_TOOLS)
    add_subdirectory(tools/vgeo_build)
    add_subdirectory(tools/vgeo_validate)
endif()

if(VGEO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Python bindings for Blender integration
if(VGEO_BUILD_PYTHON)
    add_subdirectory(src/python)
endif()
