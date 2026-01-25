# VGEO Scene Graph Library
# Multi-object management, transforms, instancing

add_library(vgeo_scene STATIC
    scene.cpp
)

target_link_libraries(vgeo_scene PUBLIC
    vgeo_core
)

target_include_directories(vgeo_scene PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/viewer  # For camera.h
)
