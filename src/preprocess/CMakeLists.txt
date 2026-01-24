# VGEO Preprocessing Library
# Mesh import, meshlet generation, hierarchy building

add_library(vgeo_preprocess STATIC
    mesh_import.cpp
    meshlet_gen.cpp
    hierarchy.cpp
    vgeo_writer.cpp
)

target_link_libraries(vgeo_preprocess PUBLIC
    vgeo_core
)

target_include_directories(vgeo_preprocess PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
