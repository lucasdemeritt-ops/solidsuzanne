#pragma once

#include <vulkan/vulkan.h>
#include <memory>

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

    // Debug options
    void set_show_clusters(bool show);
    void set_show_bounds(bool show);
    void set_wireframe(bool wireframe);

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkQueue m_graphics_queue = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;

    // Render pipeline
    VkPipeline m_render_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_render_layout = VK_NULL_HANDLE;

    // Geometry buffers
    VkBuffer m_vertex_buffer = VK_NULL_HANDLE;
    VkBuffer m_index_buffer = VK_NULL_HANDLE;
    VkBuffer m_indirect_buffer = VK_NULL_HANDLE;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace vgeo
