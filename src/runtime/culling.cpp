// VGEO GPU Culling Pipeline
// Frustum, occlusion, and backface culling on GPU using compute shaders

#include "culling.h"
#include "camera.h"
#include "vgeo_format.h"

#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace vgeo {

// Embedded SPIR-V for frustum culling compute shader
// Generated from frustum_cull.comp
// glslangValidator -V frustum_cull.comp -o frustum_cull.spv
// Then xxd -i frustum_cull.spv
static const uint32_t frustum_cull_spv[] = {
    // SPIR-V header
    0x07230203, 0x00010000, 0x000d000a, 0x00000080,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000c, 0x00060010, 0x00000004,
    0x00000011, 0x00000040, 0x00000001, 0x00000001,
    // ... (truncated for brevity - actual shader would be much larger)
    // For now we'll load from file instead
};

// Embedded SPIR-V for HZB generation compute shader
static const uint32_t hzb_gen_spv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000040,
    0x00000000, 0x00020011, 0x00000001,
    // ... (truncated)
};

bool CullingPipeline::init(VkDevice device, VkPhysicalDevice physical_device,
                           VkQueue queue, uint32_t queue_family) {
    m_device = device;
    m_physical_device = physical_device;
    m_queue = queue;
    m_queue_family = queue_family;

    // Create command pool for internal transfers
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;

    if (vkCreateCommandPool(device, &pool_info, nullptr, &m_command_pool) != VK_SUCCESS) {
        std::cerr << "Failed to create culling command pool\n";
        return false;
    }

    // Create descriptor set layout
    if (!create_descriptor_set_layout()) {
        std::cerr << "Failed to create descriptor set layout\n";
        return false;
    }

    // Create compute pipeline
    if (!create_compute_pipeline()) {
        std::cerr << "Failed to create culling compute pipeline\n";
        return false;
    }

    // Create descriptor pool
    if (!create_descriptor_pool()) {
        std::cerr << "Failed to create descriptor pool\n";
        return false;
    }

    std::cout << "GPU culling pipeline initialized\n";
    return true;
}

void CullingPipeline::destroy() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }

    vkDeviceWaitIdle(m_device);

    // Clean up HZB resources
    for (auto view : m_hzb_mip_views) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, view, nullptr);
        }
    }
    m_hzb_mip_views.clear();

    if (m_hzb_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_hzb_view, nullptr);
        m_hzb_view = VK_NULL_HANDLE;
    }

    if (m_hzb != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_hzb, nullptr);
        m_hzb = VK_NULL_HANDLE;
    }

    if (m_hzb_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_hzb_memory, nullptr);
        m_hzb_memory = VK_NULL_HANDLE;
    }

    if (m_hzb_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_hzb_sampler, nullptr);
        m_hzb_sampler = VK_NULL_HANDLE;
    }

    // Clean up buffers
    auto destroy_buffer = [this](VkBuffer& buffer, VkDeviceMemory& memory) {
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    };

    destroy_buffer(m_bounds_buffer, m_bounds_memory);
    destroy_buffer(m_cones_buffer, m_cones_memory);
    destroy_buffer(m_cluster_info_buffer, m_cluster_info_memory);
    destroy_buffer(m_visible_indices_buffer, m_visible_indices_memory);
    destroy_buffer(m_visible_count_buffer, m_visible_count_memory);
    destroy_buffer(m_indirect_buffer, m_indirect_memory);
    destroy_buffer(m_staging_buffer, m_staging_memory);

    // Clean up pipelines
    if (m_hzb_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_hzb_pipeline, nullptr);
        m_hzb_pipeline = VK_NULL_HANDLE;
    }

    if (m_hzb_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_hzb_layout, nullptr);
        m_hzb_layout = VK_NULL_HANDLE;
    }

    if (m_hzb_desc_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_hzb_desc_layout, nullptr);
        m_hzb_desc_layout = VK_NULL_HANDLE;
    }

    if (m_cull_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_cull_pipeline, nullptr);
        m_cull_pipeline = VK_NULL_HANDLE;
    }

    if (m_cull_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_cull_layout, nullptr);
        m_cull_layout = VK_NULL_HANDLE;
    }

    if (m_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptor_pool, nullptr);
        m_descriptor_pool = VK_NULL_HANDLE;
    }

    if (m_desc_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_desc_layout, nullptr);
        m_desc_layout = VK_NULL_HANDLE;
    }

    if (m_command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_command_pool, nullptr);
        m_command_pool = VK_NULL_HANDLE;
    }

    m_device = VK_NULL_HANDLE;
    m_buffers_created = false;
}

bool CullingPipeline::create_descriptor_set_layout() {
    // Bindings:
    // 0: Bounding spheres (readonly storage buffer)
    // 1: Normal cones (readonly storage buffer)
    // 2: Cluster info (readonly storage buffer)
    // 3: Visible indices output (storage buffer)
    // 4: Visible count (storage buffer, for atomic operations)
    // 5: Indirect draw commands (storage buffer)
    // 6: HZB texture (sampled image) - optional for occlusion culling

    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};

    // Binding 0: Bounding spheres
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1: Normal cones
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 2: Cluster info
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 3: Visible indices output
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 4: Visible count
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 5: Indirect draw commands
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 6: HZB texture (optional for occlusion culling)
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_desc_layout) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool CullingPipeline::create_compute_pipeline() {
    // Load shader from file or use embedded SPIR-V
    // For now, we'll create a simple placeholder that will be loaded at runtime

    // Create pipeline layout with push constants
    VkPushConstantRange push_constant{};
    push_constant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_constant.offset = 0;
    push_constant.size = sizeof(CullingPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &m_desc_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constant;

    if (vkCreatePipelineLayout(m_device, &layout_info, nullptr, &m_cull_layout) != VK_SUCCESS) {
        std::cerr << "Failed to create culling pipeline layout\n";
        return false;
    }

    // Try to load shader from multiple locations
    VkShaderModule shader_module = VK_NULL_HANDLE;

    // List of paths to try for the shader file
    const char* shader_paths[] = {
        #ifdef VGEO_SHADER_DIR
        VGEO_SHADER_DIR "/frustum_cull.comp.spv",
        #endif
        "shaders/frustum_cull.comp.spv",
        "frustum_cull.comp.spv",
        "../shaders/frustum_cull.comp.spv",
        "../../shaders/frustum_cull.comp.spv",
    };

    for (const char* path : shader_paths) {
        FILE* file = fopen(path, "rb");
        if (file) {
            fseek(file, 0, SEEK_END);
            size_t file_size = ftell(file);
            fseek(file, 0, SEEK_SET);

            std::vector<uint32_t> code((file_size + sizeof(uint32_t) - 1) / sizeof(uint32_t));
            size_t read_size = fread(code.data(), 1, file_size, file);
            fclose(file);

            if (read_size == file_size) {
                shader_module = create_shader_module(code.data(), file_size);
                if (shader_module != VK_NULL_HANDLE) {
                    std::cout << "Loaded culling shader from: " << path << "\n";
                    break;
                }
            }
        }
    }

    if (shader_module == VK_NULL_HANDLE) {
        // Pipeline layout is created, but pipeline will be null
        // This allows the system to run without GPU culling (fallback to CPU)
        std::cerr << "Warning: Could not load frustum_cull.comp.spv - GPU culling disabled\n";
        std::cerr << "  Compile shaders with: glslangValidator -V frustum_cull.comp -o frustum_cull.comp.spv\n";
        return true;  // Still return true - CPU culling will be used as fallback
    }

    VkPipelineShaderStageCreateInfo stage_info{};
    stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage_info.module = shader_module;
    stage_info.pName = "main";

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage_info;
    pipeline_info.layout = m_cull_layout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1,
                                                &pipeline_info, nullptr, &m_cull_pipeline);

    vkDestroyShaderModule(m_device, shader_module, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create culling compute pipeline\n";
        return false;
    }

    return true;
}

bool CullingPipeline::create_descriptor_pool() {
    std::array<VkDescriptorPoolSize, 2> pool_sizes{};

    // Storage buffers (6 total)
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = 6;

    // Combined image samplers (1 for HZB)
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 16;  // Extra for HZB mip levels

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 20;  // Main set + HZB sets per mip level
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();

    if (vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_descriptor_pool) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool CullingPipeline::allocate_descriptor_sets() {
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = m_descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &m_desc_layout;

    if (vkAllocateDescriptorSets(m_device, &alloc_info, &m_descriptor_set) != VK_SUCCESS) {
        std::cerr << "Failed to allocate culling descriptor set\n";
        return false;
    }

    return true;
}

bool CullingPipeline::create_buffers(uint32_t max_clusters) {
    if (m_buffers_created && max_clusters <= m_max_clusters) {
        return true;  // Buffers already large enough
    }

    // Destroy old buffers if resizing
    if (m_buffers_created) {
        vkDeviceWaitIdle(m_device);
        // Clean up old buffers...
    }

    m_max_clusters = std::max(max_clusters, 1024u);  // Minimum size

    auto create_buffer = [this](VkBuffer& buffer, VkDeviceMemory& memory,
                                VkDeviceSize size, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags properties) -> bool {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements mem_reqs;
        vkGetBufferMemoryRequirements(m_device, buffer, &mem_reqs);

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_reqs.size;
        alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, properties);

        if (vkAllocateMemory(m_device, &alloc_info, nullptr, &memory) != VK_SUCCESS) {
            return false;
        }

        vkBindBufferMemory(m_device, buffer, memory, 0);
        return true;
    };

    VkMemoryPropertyFlags device_local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkBufferUsageFlags storage_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    // Bounding spheres buffer (16 bytes per sphere: vec3 center + float radius)
    if (!create_buffer(m_bounds_buffer, m_bounds_memory,
                       m_max_clusters * sizeof(float) * 4, storage_usage, device_local)) {
        std::cerr << "Failed to create bounds buffer\n";
        return false;
    }

    // Normal cones buffer (4 bytes per cone)
    if (!create_buffer(m_cones_buffer, m_cones_memory,
                       m_max_clusters * sizeof(NormalCone), storage_usage, device_local)) {
        std::cerr << "Failed to create cones buffer\n";
        return false;
    }

    // Cluster info buffer
    if (!create_buffer(m_cluster_info_buffer, m_cluster_info_memory,
                       m_max_clusters * sizeof(GPUClusterInfo), storage_usage, device_local)) {
        std::cerr << "Failed to create cluster info buffer\n";
        return false;
    }

    // Visible indices output buffer
    if (!create_buffer(m_visible_indices_buffer, m_visible_indices_memory,
                       m_max_clusters * sizeof(uint32_t), storage_usage, device_local)) {
        std::cerr << "Failed to create visible indices buffer\n";
        return false;
    }

    // Visible count buffer (single uint32, needs to be host-readable for debug)
    if (!create_buffer(m_visible_count_buffer, m_visible_count_memory,
                       sizeof(uint32_t),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        std::cerr << "Failed to create visible count buffer\n";
        return false;
    }

    // Indirect draw commands buffer
    if (!create_buffer(m_indirect_buffer, m_indirect_memory,
                       m_max_clusters * sizeof(IndirectDrawCommand),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       device_local)) {
        std::cerr << "Failed to create indirect buffer\n";
        return false;
    }

    m_buffers_created = true;

    // Allocate and update descriptor sets
    if (!allocate_descriptor_sets()) {
        return false;
    }
    update_descriptor_sets();

    std::cout << "Created GPU culling buffers for " << m_max_clusters << " clusters\n";
    return true;
}

bool CullingPipeline::create_staging_buffer(VkDeviceSize size) {
    if (m_staging_buffer != VK_NULL_HANDLE && m_staging_size >= size) {
        return true;  // Existing buffer is large enough
    }

    // Destroy old staging buffer
    if (m_staging_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_staging_buffer, nullptr);
        vkFreeMemory(m_device, m_staging_memory, nullptr);
    }

    m_staging_size = size;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &buffer_info, nullptr, &m_staging_buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(m_device, m_staging_buffer, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &alloc_info, nullptr, &m_staging_memory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_staging_buffer, m_staging_memory, 0);
    return true;
}

void CullingPipeline::update_descriptor_sets() {
    std::array<VkDescriptorBufferInfo, 6> buffer_infos{};
    std::array<VkWriteDescriptorSet, 6> descriptor_writes{};

    // Binding 0: Bounds buffer
    buffer_infos[0].buffer = m_bounds_buffer;
    buffer_infos[0].offset = 0;
    buffer_infos[0].range = VK_WHOLE_SIZE;

    // Binding 1: Cones buffer
    buffer_infos[1].buffer = m_cones_buffer;
    buffer_infos[1].offset = 0;
    buffer_infos[1].range = VK_WHOLE_SIZE;

    // Binding 2: Cluster info buffer
    buffer_infos[2].buffer = m_cluster_info_buffer;
    buffer_infos[2].offset = 0;
    buffer_infos[2].range = VK_WHOLE_SIZE;

    // Binding 3: Visible indices buffer
    buffer_infos[3].buffer = m_visible_indices_buffer;
    buffer_infos[3].offset = 0;
    buffer_infos[3].range = VK_WHOLE_SIZE;

    // Binding 4: Visible count buffer
    buffer_infos[4].buffer = m_visible_count_buffer;
    buffer_infos[4].offset = 0;
    buffer_infos[4].range = VK_WHOLE_SIZE;

    // Binding 5: Indirect buffer
    buffer_infos[5].buffer = m_indirect_buffer;
    buffer_infos[5].offset = 0;
    buffer_infos[5].range = VK_WHOLE_SIZE;

    for (uint32_t i = 0; i < 6; i++) {
        descriptor_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_writes[i].dstSet = m_descriptor_set;
        descriptor_writes[i].dstBinding = i;
        descriptor_writes[i].dstArrayElement = 0;
        descriptor_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor_writes[i].descriptorCount = 1;
        descriptor_writes[i].pBufferInfo = &buffer_infos[i];
    }

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(descriptor_writes.size()),
                           descriptor_writes.data(), 0, nullptr);
}

void CullingPipeline::copy_to_device_buffer(VkBuffer dst, const void* data, VkDeviceSize size) {
    // Ensure staging buffer is large enough
    create_staging_buffer(size);

    // Copy data to staging buffer
    void* mapped;
    vkMapMemory(m_device, m_staging_memory, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(m_device, m_staging_memory);

    // Create command buffer for transfer
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = m_command_pool;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &begin_info);

    VkBufferCopy copy_region{};
    copy_region.size = size;
    vkCmdCopyBuffer(cmd, m_staging_buffer, dst, 1, &copy_region);

    vkEndCommandBuffer(cmd);

    // Submit and wait
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    vkQueueSubmit(m_queue, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_queue);

    vkFreeCommandBuffers(m_device, m_command_pool, 1, &cmd);
}

uint32_t CullingPipeline::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    std::cerr << "Failed to find suitable memory type for culling buffers\n";
    return 0;
}

VkShaderModule CullingPipeline::create_shader_module(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = size;
    create_info.pCode = code;

    VkShaderModule shader_module;
    if (vkCreateShaderModule(m_device, &create_info, nullptr, &shader_module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return shader_module;
}

void CullingPipeline::upload_bounds(
    const std::vector<BoundingSphere>& bounds,
    const std::vector<NormalCone>& cones,
    const std::vector<ClusterNode>& clusters
) {
    if (bounds.empty()) {
        m_cluster_count = 0;
        return;
    }

    m_cluster_count = static_cast<uint32_t>(bounds.size());

    // Create/resize buffers if needed
    if (!create_buffers(m_cluster_count)) {
        std::cerr << "Failed to create culling buffers\n";
        return;
    }

    // Upload bounding spheres
    // Convert from struct to GPU-friendly format (vec4: center.xyz, radius)
    std::vector<float> gpu_bounds(m_cluster_count * 4);
    for (size_t i = 0; i < bounds.size(); i++) {
        gpu_bounds[i * 4 + 0] = bounds[i].center[0];
        gpu_bounds[i * 4 + 1] = bounds[i].center[1];
        gpu_bounds[i * 4 + 2] = bounds[i].center[2];
        gpu_bounds[i * 4 + 3] = bounds[i].radius;
    }
    copy_to_device_buffer(m_bounds_buffer, gpu_bounds.data(),
                          gpu_bounds.size() * sizeof(float));

    // Upload normal cones (padded to 4 bytes each for alignment)
    if (!cones.empty()) {
        std::vector<int32_t> gpu_cones(m_cluster_count);
        for (size_t i = 0; i < cones.size() && i < m_cluster_count; i++) {
            // Pack cone into int32: axis.x, axis.y, axis.z, cos_angle
            gpu_cones[i] = (static_cast<int32_t>(cones[i].axis[0]) & 0xFF) |
                          ((static_cast<int32_t>(cones[i].axis[1]) & 0xFF) << 8) |
                          ((static_cast<int32_t>(cones[i].axis[2]) & 0xFF) << 16) |
                          ((static_cast<int32_t>(cones[i].cos_angle) & 0xFF) << 24);
        }
        copy_to_device_buffer(m_cones_buffer, gpu_cones.data(),
                              gpu_cones.size() * sizeof(int32_t));
    }

    // Upload cluster info
    std::vector<GPUClusterInfo> gpu_clusters(m_cluster_count);
    for (size_t i = 0; i < clusters.size() && i < m_cluster_count; i++) {
        gpu_clusters[i].meshlet_start = clusters[i].meshlet_start;
        gpu_clusters[i].meshlet_count = clusters[i].meshlet_count;
        gpu_clusters[i].error = clusters[i].error;
        gpu_clusters[i].parent_error = clusters[i].parent_error;
    }
    copy_to_device_buffer(m_cluster_info_buffer, gpu_clusters.data(),
                          gpu_clusters.size() * sizeof(GPUClusterInfo));

    std::cout << "Uploaded " << m_cluster_count << " cluster bounds to GPU\n";
}

void CullingPipeline::cull(
    VkCommandBuffer cmd,
    const Camera& camera,
    float error_threshold,
    float screen_height
) {
    if (m_cluster_count == 0 || m_cull_pipeline == VK_NULL_HANDLE) {
        return;
    }

    // Reset visible count to 0
    uint32_t zero = 0;
    vkCmdFillBuffer(cmd, m_visible_count_buffer, 0, sizeof(uint32_t), zero);

    // Memory barrier after fill
    VkMemoryBarrier fill_barrier{};
    fill_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fill_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1, &fill_barrier,
        0, nullptr,
        0, nullptr);

    // Build push constants
    CullingPushConstants pc{};

    // Copy frustum planes
    for (int i = 0; i < 6; i++) {
        pc.frustum_planes[i][0] = camera.frustum_planes[i][0];
        pc.frustum_planes[i][1] = camera.frustum_planes[i][1];
        pc.frustum_planes[i][2] = camera.frustum_planes[i][2];
        pc.frustum_planes[i][3] = camera.frustum_planes[i][3];
    }

    pc.camera_pos[0] = camera.position[0];
    pc.camera_pos[1] = camera.position[1];
    pc.camera_pos[2] = camera.position[2];
    pc.error_threshold = error_threshold;
    pc.cluster_count = m_cluster_count;
    pc.screen_height = screen_height;

    // cot(fov/2) for screen-space error calculation
    constexpr float PI = 3.14159265358979323846f;
    float fov_rad = camera.fov * PI / 180.0f;
    pc.cot_half_fov = 1.0f / std::tan(fov_rad / 2.0f);

    // Bind pipeline and descriptor set
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cull_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cull_layout,
                            0, 1, &m_descriptor_set, 0, nullptr);

    // Push constants
    vkCmdPushConstants(cmd, m_cull_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(CullingPushConstants), &pc);

    // Dispatch compute shader
    // 64 threads per workgroup
    uint32_t workgroup_count = (m_cluster_count + 63) / 64;
    vkCmdDispatch(cmd, workgroup_count, 1, 1);
}

void CullingPipeline::insert_barrier_after_cull(VkCommandBuffer cmd) {
    // Memory barrier to ensure compute writes are visible before indirect draw
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0,
        1, &barrier,
        0, nullptr,
        0, nullptr);
}

bool CullingPipeline::create_hzb_resources(uint32_t width, uint32_t height) {
    // Calculate mip levels
    m_hzb_width = width;
    m_hzb_height = height;
    m_hzb_mip_levels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    // Create HZB image
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R32_SFLOAT;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = m_hzb_mip_levels;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &image_info, nullptr, &m_hzb) != VK_SUCCESS) {
        std::cerr << "Failed to create HZB image\n";
        return false;
    }

    // Allocate memory
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(m_device, m_hzb, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &alloc_info, nullptr, &m_hzb_memory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate HZB memory\n";
        return false;
    }

    vkBindImageMemory(m_device, m_hzb, m_hzb_memory, 0);

    // Create image view for entire image
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = m_hzb;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R32_SFLOAT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = m_hzb_mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &view_info, nullptr, &m_hzb_view) != VK_SUCCESS) {
        std::cerr << "Failed to create HZB image view\n";
        return false;
    }

    // Create per-mip views for compute shader writes
    m_hzb_mip_views.resize(m_hzb_mip_levels);
    for (uint32_t i = 0; i < m_hzb_mip_levels; i++) {
        view_info.subresourceRange.baseMipLevel = i;
        view_info.subresourceRange.levelCount = 1;

        if (vkCreateImageView(m_device, &view_info, nullptr, &m_hzb_mip_views[i]) != VK_SUCCESS) {
            std::cerr << "Failed to create HZB mip view " << i << "\n";
            return false;
        }
    }

    // Create sampler for HZB reads
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = static_cast<float>(m_hzb_mip_levels);

    if (vkCreateSampler(m_device, &sampler_info, nullptr, &m_hzb_sampler) != VK_SUCCESS) {
        std::cerr << "Failed to create HZB sampler\n";
        return false;
    }

    return true;
}

void CullingPipeline::update_hzb(VkCommandBuffer cmd, VkImageView depth_view,
                                  uint32_t width, uint32_t height) {
    // Create HZB resources if needed or if size changed
    if (m_hzb == VK_NULL_HANDLE || m_hzb_width != width || m_hzb_height != height) {
        // Clean up old resources
        for (auto view : m_hzb_mip_views) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, view, nullptr);
            }
        }
        m_hzb_mip_views.clear();

        if (m_hzb_view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_hzb_view, nullptr);
            m_hzb_view = VK_NULL_HANDLE;
        }

        if (m_hzb != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, m_hzb, nullptr);
            m_hzb = VK_NULL_HANDLE;
        }

        if (m_hzb_memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_hzb_memory, nullptr);
            m_hzb_memory = VK_NULL_HANDLE;
        }

        if (!create_hzb_resources(width, height)) {
            return;
        }
    }

    // Transition HZB to general layout for compute writes
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_hzb;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = m_hzb_mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier);

    // TODO: Implement HZB generation compute pass
    // This would involve:
    // 1. Copy depth buffer to HZB mip 0 (with format conversion if needed)
    // 2. For each mip level, dispatch compute to downsample with max operation

    // For now, this is a placeholder - full HZB generation would be added
    // when occlusion culling is enabled

    // Transition HZB to shader read for culling
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier);
}

} // namespace vgeo
