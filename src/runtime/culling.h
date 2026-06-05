#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

namespace vgeo {

struct Camera;
struct BoundingSphere;
struct NormalCone;
struct ClusterNode;

// GPU-side cluster data for culling
struct GPUClusterInfo {
    uint32_t meshlet_start;
    uint32_t meshlet_count;
    float error;
    float parent_error;
};

// Push constants for culling compute shader
struct CullingPushConstants {
    float frustum_planes[6][4];  // 6 planes, each (nx, ny, nz, d)
    float camera_pos[3];
    float error_threshold;
    uint32_t cluster_count;
    float screen_height;
    float cot_half_fov;
    uint32_t _padding;
};

// Indirect draw command (matches VkDrawIndexedIndirectCommand)
struct IndirectDrawCommand {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t vertex_offset;
    uint32_t first_instance;
};

class CullingPipeline {
public:
    bool init(VkDevice device, VkPhysicalDevice physical_device, VkQueue queue, uint32_t queue_family);
    void destroy();

    // Upload cluster bounds for culling
    void upload_bounds(
        const std::vector<BoundingSphere>& bounds,
        const std::vector<NormalCone>& cones,
        const std::vector<ClusterNode>& clusters
    );

    // Run culling pass, output visible indices
    void cull(
        VkCommandBuffer cmd,
        const Camera& camera,
        float error_threshold,
        float screen_height
    );

    // Update HZB from depth buffer
    void update_hzb(VkCommandBuffer cmd, VkImageView depth_view, uint32_t width, uint32_t height);

    // Get buffers for rendering
    VkBuffer get_visible_indices_buffer() const { return m_visible_indices_buffer; }
    VkBuffer get_visible_count_buffer() const { return m_visible_count_buffer; }
    VkBuffer get_indirect_buffer() const { return m_indirect_buffer; }

    // Get descriptor set for use in rendering
    VkDescriptorSet get_descriptor_set() const { return m_descriptor_set; }

    // Stats (read back after culling)
    uint32_t get_visible_count() const { return m_last_visible_count; }

    // Memory barrier helper for transitioning after cull
    void insert_barrier_after_cull(VkCommandBuffer cmd);

private:
    // Vulkan resource creation helpers
    bool create_descriptor_set_layout();
    bool create_compute_pipeline();
    bool create_hzb_pipeline();
    bool create_descriptor_pool();
    bool allocate_descriptor_sets();
    bool create_buffers(uint32_t max_clusters);
    bool create_staging_buffer(VkDeviceSize size);
    bool create_hzb_resources(uint32_t width, uint32_t height);

    void update_descriptor_sets();
    void copy_to_device_buffer(VkBuffer dst, const void* data, VkDeviceSize size);

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
    VkShaderModule create_shader_module(const uint32_t* code, size_t size);

    // Core Vulkan objects
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queue_family = 0;

    // Command pool for internal transfers
    VkCommandPool m_command_pool = VK_NULL_HANDLE;

    // Culling pipeline
    VkPipeline m_cull_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_cull_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_desc_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptor_set = VK_NULL_HANDLE;

    // HZB generation pipeline
    VkPipeline m_hzb_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_hzb_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_hzb_desc_layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_hzb_descriptor_sets;
    VkSampler m_hzb_sampler = VK_NULL_HANDLE;

    // Input buffers (cluster data)
    VkBuffer m_bounds_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_bounds_memory = VK_NULL_HANDLE;

    VkBuffer m_cones_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_cones_memory = VK_NULL_HANDLE;

    VkBuffer m_cluster_info_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_cluster_info_memory = VK_NULL_HANDLE;

    // Output buffers
    VkBuffer m_visible_indices_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_visible_indices_memory = VK_NULL_HANDLE;

    VkBuffer m_visible_count_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_visible_count_memory = VK_NULL_HANDLE;

    VkBuffer m_indirect_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_indirect_memory = VK_NULL_HANDLE;

    // Staging buffer for uploads
    VkBuffer m_staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_staging_memory = VK_NULL_HANDLE;
    VkDeviceSize m_staging_size = 0;

    // HZB (Hierarchical Z-Buffer)
    VkImage m_hzb = VK_NULL_HANDLE;
    VkDeviceMemory m_hzb_memory = VK_NULL_HANDLE;
    VkImageView m_hzb_view = VK_NULL_HANDLE;
    std::vector<VkImageView> m_hzb_mip_views;
    uint32_t m_hzb_width = 0;
    uint32_t m_hzb_height = 0;
    uint32_t m_hzb_mip_levels = 0;

    // State
    uint32_t m_cluster_count = 0;
    uint32_t m_max_clusters = 0;
    uint32_t m_last_visible_count = 0;
    bool m_buffers_created = false;
};

} // namespace vgeo
