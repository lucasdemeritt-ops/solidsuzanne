# VGEO Runtime Library
# Vulkan renderer, culling, streaming

add_library(vgeo_runtime STATIC
    vgeo_loader.cpp
    cluster_manager.cpp
    culling.cpp
    renderer.cpp
)

target_link_libraries(vgeo_runtime PUBLIC
    vgeo_core
    Vulkan::Vulkan
    glfw
)

target_include_directories(vgeo_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src/viewer
)

target_include_directories(vgeo_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
