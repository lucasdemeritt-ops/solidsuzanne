#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <vector>

namespace vgeo {

class VGeoAsset;
class ClusterManager;
struct Camera;

class Renderer {
public:
    bool init(void* window_handle, uint32_t width, uint32_t height);
    void destroy();

    // Resize swapchain
    void resize(uint32_t width, uint32_t height);

    // Upload asset to GPU
    void upload_asset(const VGeoAsset& asset);

    // Render frame
    void render(const Camera& camera, const ClusterManager& clusters);

    // Render frame with model transform
    void render(const Camera& camera, const ClusterManager& clusters, const float* model_matrix);

    // Render hardcoded triangle (for testing)
    void render_triangle(const Camera& camera);

    // Debug options
    void set_show_clusters(bool show);
    void set_show_bounds(bool show);
    void set_wireframe(bool wireframe);

    // Check if asset is loaded
    bool has_asset() const { return m_has_asset; }

private:
    bool create_instance();
    bool create_surface(void* window_handle);
    bool select_physical_device();
    bool create_device();
    bool create_swapchain();
    bool create_render_pass();
    bool create_framebuffers();
    bool create_command_pool();
    bool create_sync_objects();
    bool create_pipeline();
    bool create_triangle_buffers();

    void cleanup_swapchain();
    void recreate_swapchain();

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
    VkShaderModule create_shader_module(const uint32_t* code, size_t size);

    // Core Vulkan objects
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkQueue m_graphics_queue = VK_NULL_HANDLE;
    VkQueue m_present_queue = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;

    // Swapchain
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_swapchain_images;
    std::vector<VkImageView> m_swapchain_image_views;
    VkFormat m_swapchain_format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapchain_extent = {0, 0};

    // Depth buffer
    VkImage m_depth_image = VK_NULL_HANDLE;
    VkDeviceMemory m_depth_memory = VK_NULL_HANDLE;
    VkImageView m_depth_view = VK_NULL_HANDLE;

    // Render pass and framebuffers
    VkRenderPass m_render_pass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;

    // Command buffers
    VkCommandPool m_command_pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_command_buffers;

    // Synchronization
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
    std::vector<VkSemaphore> m_image_available_semaphores;
    std::vector<VkSemaphore> m_render_finished_semaphores;
    std::vector<VkFence> m_in_flight_fences;
    uint32_t m_current_frame = 0;

    // Render pipeline
    VkPipeline m_render_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_render_layout = VK_NULL_HANDLE;

    // Triangle buffers (for testing)
    VkBuffer m_triangle_vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_triangle_vertex_memory = VK_NULL_HANDLE;

    // Geometry buffers (for actual meshes)
    VkBuffer m_vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertex_memory = VK_NULL_HANDLE;
    VkBuffer m_index_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_index_memory = VK_NULL_HANDLE;
    VkBuffer m_indirect_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_indirect_memory = VK_NULL_HANDLE;

    uint32_t m_vertex_count = 0;
    uint32_t m_index_count = 0;

    // Queue family indices
    uint32_t m_graphics_family = 0;
    uint32_t m_present_family = 0;

    // Window dimensions
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // State flags
    bool m_has_asset = false;
    bool m_show_clusters = false;
    bool m_show_bounds = false;
    bool m_wireframe = false;
    bool m_framebuffer_resized = false;

    // Validation layers
#ifdef NDEBUG
    static constexpr bool ENABLE_VALIDATION = false;
#else
    static constexpr bool ENABLE_VALIDATION = true;
#endif
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
};

} // namespace vgeo
