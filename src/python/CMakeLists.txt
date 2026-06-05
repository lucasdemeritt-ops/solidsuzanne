# VGEO Python Bindings
# Exposes renderer and scene graph to Python for Blender integration

# Find Python
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# Fetch pybind11
include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.11.1
)
FetchContent_MakeAvailable(pybind11)

# Create the Python module
pybind11_add_module(vgeo_python MODULE
    bindings.cpp
)

target_link_libraries(vgeo_python PRIVATE
    vgeo_runtime
    vgeo_scene
    vgeo_core
    Vulkan::Vulkan
)

target_include_directories(vgeo_python PRIVATE
    ${CMAKE_SOURCE_DIR}/src/runtime
    ${CMAKE_SOURCE_DIR}/src/scene
    ${CMAKE_SOURCE_DIR}/src/core
)

# Output name without lib prefix
set_target_properties(vgeo_python PROPERTIES
    OUTPUT_NAME "vgeo_native"
    PREFIX ""
)

# On Windows, use .pyd extension
if(WIN32)
    set_target_properties(vgeo_python PROPERTIES SUFFIX ".pyd")
endif()
