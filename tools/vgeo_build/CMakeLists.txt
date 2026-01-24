# vgeo_build CLI tool
# Convert meshes to .vgeo format

add_executable(vgeo_build
    main.cpp
)

target_link_libraries(vgeo_build PRIVATE
    vgeo_preprocess
    vgeo_core
)
