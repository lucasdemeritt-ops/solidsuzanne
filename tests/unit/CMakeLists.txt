# Unit tests

# Pipeline integration test
add_executable(test_pipeline test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE
    vgeo_preprocess
    vgeo_runtime
    vgeo_core
)
add_test(NAME pipeline COMMAND test_pipeline
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../)

# Copy test assets to build directory
add_custom_command(TARGET test_pipeline POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../assets
    ${CMAKE_CURRENT_BINARY_DIR}/../assets
)
