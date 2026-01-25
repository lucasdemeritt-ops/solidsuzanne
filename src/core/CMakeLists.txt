# VGEO Core Library
# Shared types, math, utilities, and file loading

add_library(vgeo_core STATIC
    vgeo_loader.cpp
)

target_include_directories(vgeo_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# C++20 required
target_compile_features(vgeo_core PUBLIC cxx_std_20)
