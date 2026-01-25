# VGEO Standalone Viewer
# Vulkan window, camera, debug UI

add_executable(vgeo_viewer
    main.cpp
    window.cpp
)

target_link_libraries(vgeo_viewer PRIVATE
    vgeo_runtime
    vgeo_scene
    vgeo_core
    glfw
    Vulkan::Vulkan
)

target_include_directories(vgeo_viewer PRIVATE
    ${CMAKE_SOURCE_DIR}/src/runtime
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/scene
)
