# vgeo_validate CLI tool
# Validate .vgeo files

add_executable(vgeo_validate
    main.cpp
)

target_link_libraries(vgeo_validate PRIVATE
    vgeo_core
)
