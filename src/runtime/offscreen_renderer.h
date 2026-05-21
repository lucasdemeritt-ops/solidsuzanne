#pragma once

// VGEO Offscreen Renderer
// Vulkan renderer for headless/offscreen rendering with OpenGL interop

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>

#include <memory>
#include <vector>
#include <cstdint>

namespace vgeo {

struct VGeoAsset;
class ClusterManager;
struct Camera;

// Handle for shared memory (platform-specific)
#ifdef _WIN32
using SharedMemoryHandle = HANDLE;
#else
using SharedMemoryHandle = int;  // File descriptor on Linux
#endif

// Information about a shared image for OpenGL import
struct SharedImageInfo {
    SharedMemoryHandle handle;      // OS handle to shared memory
    uint32_t width;
    uint32_t height;
    uint64_t allocation_size;       // Total memory size
    uint32_t memory_type_index;     // Vulkan memory type
    bool valid;
};

class OffscreenRenderer {
public:
    OffscreenRenderer();
    ~OffscreenRenderer();

    // No copy
    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;

    // Initialize Vulkan (headless, no window)
    bool init(uint32_t width, uint32_t height);

    // Resize the render target
    bool resize(uint32_t width, uint32_t height);

    // Cleanup
    void destroy();

    // Upload asset to GPU
    void upload_asset(const VGeoAsset& asset);

    // Render a frame
    void render(const Camera& camera, const ClusterManager& clusters, const float* model_matrix = nullptr);

    // Get shared image info for OpenGL import
    SharedImageInfo get_shared_image_info() const;

    // Get raw pixel data (for fallback/debugging)
    // Returns RGBA8 pixels, caller must provide buffer of size width*height*4
    bool read_pixels(uint8_t* out_pixels);

    // Get dimensions
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }

    // Check if initialized
    bool is_initialized() const { return m_initialized; }

private:
    bool create_instance();
    bool select_physical_device();
    bool create_device();
    bool create_render_target();
    bool create_render_pass();
    bool create_framebuffer();
    bool create_command_pool();
    bool create_sync_objects();
    bool create_pipeline();

    void cleanup_render_target();

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
    VkShaderModule create_shader_module(const uint32_t* code, size_t size);

    // Vulkan objects
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphics_queue = VK_NULL_HANDLE;

    // Render target (exportable for OpenGL)
    VkImage m_color_image = VK_NULL_HANDLE;
    VkDeviceMemory m_color_memory = VK_NULL_HANDLE;
    VkImageView m_color_view = VK_NULL_HANDLE;
    SharedMemoryHandle m_color_handle = nullptr;  // For OpenGL sharing

    VkImage m_depth_image = VK_NULL_HANDLE;
    VkDeviceMemory m_depth_memory = VK_NULL_HANDLE;
    VkImageView m_depth_view = VK_NULL_HANDLE;

    // Staging buffer for pixel readback
    VkBuffer m_staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_staging_memory = VK_NULL_HANDLE;

    // Render pass and framebuffer
    VkRenderPass m_render_pass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;

    // Command buffer
    VkCommandPool m_command_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_command_buffer = VK_NULL_HANDLE;

    // Sync
    VkFence m_render_fence = VK_NULL_HANDLE;

    // Pipeline
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;

    // Geometry buffers
    VkBuffer m_vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertex_memory = VK_NULL_HANDLE;
    VkBuffer m_index_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_index_memory = VK_NULL_HANDLE;

    uint32_t m_vertex_count = 0;
    uint32_t m_index_count = 0;

    // Queue family
    uint32_t m_graphics_family = 0;

    // Dimensions
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // State
    bool m_initialized = false;
    bool m_has_asset = false;

    // Extension function pointers
    PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32HandleKHR = nullptr;
};

} // namespace vgeo
