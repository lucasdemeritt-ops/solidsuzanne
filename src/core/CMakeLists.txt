# VGEO Core Library
# Shared types, math, and utilities

add_library(vgeo_core INTERFACE)

target_include_directories(vgeo_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Header-only for now, will add sources as needed
