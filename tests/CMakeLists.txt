# VGEO Tests

add_subdirectory(unit)

# Copy test assets
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/assets
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
