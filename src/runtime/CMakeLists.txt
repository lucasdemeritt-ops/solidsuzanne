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

# ============================================================================
# Shader Compilation
# ============================================================================

# Find glslangValidator or glslc for shader compilation
find_program(GLSLANG_VALIDATOR glslangValidator HINTS $ENV{VULKAN_SDK}/Bin)
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin)

if(GLSLANG_VALIDATOR)
    set(SHADER_COMPILER ${GLSLANG_VALIDATOR})
    set(SHADER_COMPILER_FLAGS -V)
elseif(GLSLC)
    set(SHADER_COMPILER ${GLSLC})
    set(SHADER_COMPILER_FLAGS "")
else()
    message(WARNING "No SPIR-V shader compiler found (glslangValidator or glslc). Shaders will not be compiled.")
endif()

# Shader source files
set(SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/basic.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/basic.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/frustum_cull.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hzb_generate.comp
)

# Output directory for compiled shaders
set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

if(SHADER_COMPILER)
    set(SPIRV_OUTPUTS)

    foreach(SHADER_SOURCE ${SHADER_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME)
        set(SPIRV_OUTPUT ${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv)

        add_custom_command(
            OUTPUT ${SPIRV_OUTPUT}
            COMMAND ${SHADER_COMPILER} ${SHADER_COMPILER_FLAGS} -o ${SPIRV_OUTPUT} ${SHADER_SOURCE}
            DEPENDS ${SHADER_SOURCE}
            COMMENT "Compiling shader ${SHADER_NAME}"
            VERBATIM
        )

        list(APPEND SPIRV_OUTPUTS ${SPIRV_OUTPUT})
    endforeach()

    # Create a target for all shaders
    add_custom_target(vgeo_shaders ALL DEPENDS ${SPIRV_OUTPUTS})

    # Make runtime library depend on shaders
    add_dependencies(vgeo_runtime vgeo_shaders)

    # Define shader output directory as a compile definition
    target_compile_definitions(vgeo_runtime PRIVATE
        VGEO_SHADER_DIR="${SHADER_OUTPUT_DIR}"
    )
endif()

# Copy shaders to build directory for runtime access
add_custom_command(TARGET vgeo_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMENT "Creating shader output directory"
)
