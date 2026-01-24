#pragma once

#include <vulkan/vulkan.h>
#include <vector>

namespace vgeo {

struct Camera;
struct BoundingSphere;
struct NormalCone;

class CullingPipeline {
public:
    bool init(VkDevice device, VkPhysicalDevice physical_device);
    void destroy();

    // Upload cluster bounds for culling
    void upload_bounds(
        const std::vector<BoundingSphere>& bounds,
        const std::vector<NormalCone>& cones
    );

    // Run culling pass, output visible indices
    void cull(
        VkCommandBuffer cmd,
        const Camera& camera,
        VkBuffer output_indices,
        VkBuffer output_count
    );

    // Update HZB from depth buffer
    void update_hzb(VkCommandBuffer cmd, VkImageView depth_view);

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkPipeline m_cull_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_cull_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_desc_layout = VK_NULL_HANDLE;

    VkBuffer m_bounds_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_bounds_memory = VK_NULL_HANDLE;

    VkImage m_hzb = VK_NULL_HANDLE;
    VkImageView m_hzb_view = VK_NULL_HANDLE;
};

} // namespace vgeo
