# VGEO Standalone Viewer
# Vulkan window, camera, debug UI

add_executable(vgeo_viewer
    main.cpp
    window.cpp
    camera.cpp
)

target_link_libraries(vgeo_viewer PRIVATE
    vgeo_runtime
    vgeo_core
)
