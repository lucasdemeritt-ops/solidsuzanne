// VGEO Offscreen Renderer
// Headless Vulkan rendering with OpenGL interop via external memory

#include "offscreen_renderer.h"
#include "vgeo_loader.h"
#include "cluster_manager.h"
#include "camera.h"

#include <iostream>
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <array>
#include <filesystem>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace vgeo {

// Shader loading (same as main renderer)
static std::string find_shader_dir() {
    std::vector<std::string> search_paths = {
        "shaders",
        "../shaders",
        "../../shaders",
        "../../../shaders",
#ifdef VGEO_SHADER_DIR
        VGEO_SHADER_DIR,
#endif
    };

    for (const auto& path : search_paths) {
        if (std::filesystem::exists(path + "/basic.vert.spv")) {
            return path;
        }
    }
    return "shaders";
}

static std::vector<uint32_t> load_shader_file(const std::string& filename) {
    std::string shader_dir = find_shader_dir();
    std::string full_path = shader_dir + "/" + filename;

    std::ifstream file(full_path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open shader: " << full_path << "\n";
        return {};
    }

    size_t file_size = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> buffer(file_size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    return buffer;
}

OffscreenRenderer::OffscreenRenderer() = default;

OffscreenRenderer::~OffscreenRenderer() {
    destroy();
}

bool OffscreenRenderer::init(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    if (!create_instance()) return false;
    if (!select_physical_device()) return false;
    if (!create_device()) return false;
    if (!create_render_target()) return false;
    if (!create_render_pass()) return false;
    if (!create_framebuffer()) return false;
    if (!create_command_pool()) return false;
    if (!create_sync_objects()) return false;
    if (!create_pipeline()) return false;

    m_initialized = true;
    std::cout << "Offscreen renderer initialized: " << width << "x" << height << "\n";
    return true;
}

bool OffscreenRenderer::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return true;

    vkDeviceWaitIdle(m_device);

    m_width = width;
    m_height = height;

    cleanup_render_target();

    if (!create_render_target()) return false;
    if (!create_framebuffer()) return false;

    return true;
}

void OffscreenRenderer::destroy() {
    if (!m_device) {
        // Already destroyed or never initialized
        return;
    }

    vkDeviceWaitIdle(m_device);

    // Cleanup in reverse order, resetting handles to prevent double-free
    if (m_pipeline) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipeline_layout) {
        vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
        m_pipeline_layout = VK_NULL_HANDLE;
    }

    if (m_vertex_buffer) {
        vkDestroyBuffer(m_device, m_vertex_buffer, nullptr);
        m_vertex_buffer = VK_NULL_HANDLE;
    }
    if (m_vertex_memory) {
        vkFreeMemory(m_device, m_vertex_memory, nullptr);
        m_vertex_memory = VK_NULL_HANDLE;
    }
    if (m_index_buffer) {
        vkDestroyBuffer(m_device, m_index_buffer, nullptr);
        m_index_buffer = VK_NULL_HANDLE;
    }
    if (m_index_memory) {
        vkFreeMemory(m_device, m_index_memory, nullptr);
        m_index_memory = VK_NULL_HANDLE;
    }

    if (m_render_fence) {
        vkDestroyFence(m_device, m_render_fence, nullptr);
        m_render_fence = VK_NULL_HANDLE;
    }
    if (m_command_pool) {
        vkDestroyCommandPool(m_device, m_command_pool, nullptr);
        m_command_pool = VK_NULL_HANDLE;
    }

    cleanup_render_target();

    if (m_render_pass) {
        vkDestroyRenderPass(m_device, m_render_pass, nullptr);
        m_render_pass = VK_NULL_HANDLE;
    }

    vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;

    if (m_instance) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    m_initialized = false;
    m_has_asset = false;
}

void OffscreenRenderer::cleanup_render_target() {
    if (m_framebuffer) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }

    if (m_color_view) vkDestroyImageView(m_device, m_color_view, nullptr);
    if (m_color_image) vkDestroyImage(m_device, m_color_image, nullptr);
    if (m_color_memory) vkFreeMemory(m_device, m_color_memory, nullptr);
#ifdef _WIN32
    if (m_color_handle) CloseHandle(m_color_handle);
#else
    if (m_color_handle >= 0) close(m_color_handle);
#endif
    m_color_view = VK_NULL_HANDLE;
    m_color_image = VK_NULL_HANDLE;
    m_color_memory = VK_NULL_HANDLE;
    m_color_handle = INVALID_SHARED_MEMORY_HANDLE;

    if (m_depth_view) vkDestroyImageView(m_device, m_depth_view, nullptr);
    if (m_depth_image) vkDestroyImage(m_device, m_depth_image, nullptr);
    if (m_depth_memory) vkFreeMemory(m_device, m_depth_memory, nullptr);
    m_depth_view = VK_NULL_HANDLE;
    m_depth_image = VK_NULL_HANDLE;
    m_depth_memory = VK_NULL_HANDLE;

    if (m_staging_buffer) vkDestroyBuffer(m_device, m_staging_buffer, nullptr);
    if (m_staging_memory) vkFreeMemory(m_device, m_staging_memory, nullptr);
    m_staging_buffer = VK_NULL_HANDLE;
    m_staging_memory = VK_NULL_HANDLE;
}

bool OffscreenRenderer::create_instance() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "VGEO Offscreen";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "VGEO";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    // Extensions for external memory
    std::vector<const char*> extensions = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    };

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&create_info, nullptr, &m_instance) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance\n";
        return false;
    }

    return true;
}

bool OffscreenRenderer::select_physical_device() {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr);

    if (device_count == 0) {
        std::cerr << "No Vulkan devices found\n";
        return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data());

    // Pick first suitable device
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        // Find graphics queue family
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);

        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

        for (uint32_t i = 0; i < queue_family_count; i++) {
            if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                m_graphics_family = i;
                m_physical_device = device;
                std::cout << "Using GPU: " << props.deviceName << "\n";
                return true;
            }
        }
    }

    std::cerr << "No suitable GPU found\n";
    return false;
}

bool OffscreenRenderer::create_device() {
    float queue_priority = 1.0f;

    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = m_graphics_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    // Device extensions for external memory
    std::vector<const char*> extensions = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
#else
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
#endif
    };

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
    create_info.pEnabledFeatures = &features;

    if (vkCreateDevice(m_physical_device, &create_info, nullptr, &m_device) != VK_SUCCESS) {
        std::cerr << "Failed to create logical device\n";
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphics_family, 0, &m_graphics_queue);

    // Load extension functions
#ifdef _WIN32
    vkGetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR)
        vkGetDeviceProcAddr(m_device, "vkGetMemoryWin32HandleKHR");
#else
    vkGetMemoryFdKHR_fn = (PFN_vkGetMemoryFdKHR)
        vkGetDeviceProcAddr(m_device, "vkGetMemoryFdKHR");
#endif

    return true;
}

bool OffscreenRenderer::create_render_target() {
    // === Color image (exportable for OpenGL) ===
    VkExternalMemoryImageCreateInfo external_info{};
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
#ifdef _WIN32
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_info;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {m_width, m_height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &image_info, nullptr, &m_color_image) != VK_SUCCESS) {
        std::cerr << "Failed to create color image\n";
        return false;
    }

    // Allocate exportable memory
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(m_device, m_color_image, &mem_reqs);

    VkExportMemoryAllocateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
#ifdef _WIN32
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &export_info;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &alloc_info, nullptr, &m_color_memory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate color image memory\n";
        return false;
    }

    vkBindImageMemory(m_device, m_color_image, m_color_memory, 0);

    // Get the exportable handle
#ifdef _WIN32
    if (vkGetMemoryWin32HandleKHR) {
        VkMemoryGetWin32HandleInfoKHR handle_info{};
        handle_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
        handle_info.memory = m_color_memory;
        handle_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        vkGetMemoryWin32HandleKHR(m_device, &handle_info, &m_color_handle);
    }
#else
    if (vkGetMemoryFdKHR_fn) {
        VkMemoryGetFdInfoKHR handle_info{};
        handle_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        handle_info.memory = m_color_memory;
        handle_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        vkGetMemoryFdKHR_fn(m_device, &handle_info, &m_color_handle);
    }
#endif

    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = m_color_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &view_info, nullptr, &m_color_view) != VK_SUCCESS) {
        std::cerr << "Failed to create color image view\n";
        return false;
    }

    // === Depth image (not exported) ===
    VkImageCreateInfo depth_info{};
    depth_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_info.imageType = VK_IMAGE_TYPE_2D;
    depth_info.format = VK_FORMAT_D32_SFLOAT;
    depth_info.extent = {m_width, m_height, 1};
    depth_info.mipLevels = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    if (vkCreateImage(m_device, &depth_info, nullptr, &m_depth_image) != VK_SUCCESS) {
        std::cerr << "Failed to create depth image\n";
        return false;
    }

    vkGetImageMemoryRequirements(m_device, m_depth_image, &mem_reqs);

    VkMemoryAllocateInfo depth_alloc{};
    depth_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depth_alloc.allocationSize = mem_reqs.size;
    depth_alloc.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &depth_alloc, nullptr, &m_depth_memory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate depth memory\n";
        return false;
    }

    vkBindImageMemory(m_device, m_depth_image, m_depth_memory, 0);

    VkImageViewCreateInfo depth_view_info{};
    depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_info.image = m_depth_image;
    depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_info.format = VK_FORMAT_D32_SFLOAT;
    depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_view_info.subresourceRange.baseMipLevel = 0;
    depth_view_info.subresourceRange.levelCount = 1;
    depth_view_info.subresourceRange.baseArrayLayer = 0;
    depth_view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &depth_view_info, nullptr, &m_depth_view) != VK_SUCCESS) {
        std::cerr << "Failed to create depth image view\n";
        return false;
    }

    // === Staging buffer for pixel readback ===
    VkBufferCreateInfo staging_info{};
    staging_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_info.size = m_width * m_height * 4;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    if (vkCreateBuffer(m_device, &staging_info, nullptr, &m_staging_buffer) != VK_SUCCESS) {
        std::cerr << "Failed to create staging buffer\n";
        return false;
    }

    vkGetBufferMemoryRequirements(m_device, m_staging_buffer, &mem_reqs);

    VkMemoryAllocateInfo staging_alloc{};
    staging_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    staging_alloc.allocationSize = mem_reqs.size;
    staging_alloc.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &staging_alloc, nullptr, &m_staging_memory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate staging memory\n";
        return false;
    }

    vkBindBufferMemory(m_device, m_staging_buffer, m_staging_memory, 0);

    return true;
}

bool OffscreenRenderer::create_render_pass() {
    VkAttachmentDescription color_attachment{};
    color_attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentDescription depth_attachment{};
    depth_attachment.format = VK_FORMAT_D32_SFLOAT;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_ref{};
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    std::array<VkAttachmentDescription, 2> attachments = {color_attachment, depth_attachment};

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = static_cast<uint32_t>(attachments.size());
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;

    if (vkCreateRenderPass(m_device, &render_pass_info, nullptr, &m_render_pass) != VK_SUCCESS) {
        std::cerr << "Failed to create render pass\n";
        return false;
    }

    return true;
}

bool OffscreenRenderer::create_framebuffer() {
    std::array<VkImageView, 2> attachments = {m_color_view, m_depth_view};

    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = m_render_pass;
    fb_info.attachmentCount = static_cast<uint32_t>(attachments.size());
    fb_info.pAttachments = attachments.data();
    fb_info.width = m_width;
    fb_info.height = m_height;
    fb_info.layers = 1;

    if (vkCreateFramebuffer(m_device, &fb_info, nullptr, &m_framebuffer) != VK_SUCCESS) {
        std::cerr << "Failed to create framebuffer\n";
        return false;
    }

    return true;
}

bool OffscreenRenderer::create_command_pool() {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = m_graphics_family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(m_device, &pool_info, nullptr, &m_command_pool) != VK_SUCCESS) {
        std::cerr << "Failed to create command pool\n";
        return false;
    }

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = m_command_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(m_device, &alloc_info, &m_command_buffer) != VK_SUCCESS) {
        std::cerr << "Failed to allocate command buffer\n";
        return false;
    }

    return true;
}

bool OffscreenRenderer::create_sync_objects() {
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (vkCreateFence(m_device, &fence_info, nullptr, &m_render_fence) != VK_SUCCESS) {
        std::cerr << "Failed to create fence\n";
        return false;
    }

    return true;
}

bool OffscreenRenderer::create_pipeline() {
    // Load shaders
    auto vert_code = load_shader_file("basic.vert.spv");
    auto frag_code = load_shader_file("basic.frag.spv");

    if (vert_code.empty() || frag_code.empty()) {
        std::cerr << "Failed to load shaders\n";
        return false;
    }

    VkShaderModule vert_module = create_shader_module(vert_code.data(), vert_code.size() * sizeof(uint32_t));
    VkShaderModule frag_module = create_shader_module(frag_code.data(), frag_code.size() * sizeof(uint32_t));

    VkPipelineShaderStageCreateInfo vert_stage{};
    vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module;
    vert_stage.pName = "main";

    VkPipelineShaderStageCreateInfo frag_stage{};
    frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage.module = frag_module;
    frag_stage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vert_stage, frag_stage};

    // Vertex input: pos(3f) + normal(3f) + meshlet_id(1u) = 28 bytes
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 6 * sizeof(float) + sizeof(uint32_t);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attrs{};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = 0;

    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = 3 * sizeof(float);

    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32_UINT;
    attrs[2].offset = 6 * sizeof(float);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertex_input.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend_attachment;

    // Dynamic state for viewport/scissor
    std::vector<VkDynamicState> dynamic_states = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    // Push constants: 2x mat4 + uint debug_mode
    VkPushConstantRange push_constant{};
    push_constant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_constant.offset = 0;
    push_constant.size = sizeof(float) * 32 + sizeof(uint32_t);  // 132 bytes

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constant;

    if (vkCreatePipelineLayout(m_device, &layout_info, nullptr, &m_pipeline_layout) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout\n";
        return false;
    }

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = m_pipeline_layout;
    pipeline_info.renderPass = m_render_pass;
    pipeline_info.subpass = 0;

    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipeline) != VK_SUCCESS) {
        std::cerr << "Failed to create graphics pipeline\n";
        vkDestroyShaderModule(m_device, vert_module, nullptr);
        vkDestroyShaderModule(m_device, frag_module, nullptr);
        return false;
    }

    vkDestroyShaderModule(m_device, vert_module, nullptr);
    vkDestroyShaderModule(m_device, frag_module, nullptr);

    return true;
}

uint32_t OffscreenRenderer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return 0;
}

VkShaderModule OffscreenRenderer::create_shader_module(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = size;
    create_info.pCode = code;

    VkShaderModule module;
    if (vkCreateShaderModule(m_device, &create_info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

void OffscreenRenderer::upload_asset(const VGeoAsset& asset) {
    size_t vertex_count = asset.positions.size() / 3;

    // Build vertex -> meshlet mapping for debug color mode
    std::vector<uint32_t> vertex_meshlet_ids(vertex_count, 0);
    for (uint32_t m = 0; m < asset.meshlets.size(); m++) {
        const auto& meshlet = asset.meshlets[m];
        uint32_t idx_end = meshlet.index_offset + meshlet.triangle_count * 3;
        for (uint32_t i = meshlet.index_offset; i < idx_end && i < asset.indices.size(); i++) {
            uint32_t v = asset.indices[i];
            if (v < vertex_count) vertex_meshlet_ids[v] = m;
        }
    }

    // Interleaved: pos(3f) + normal(3f) + meshlet_id(1u) = 28 bytes per vertex
    struct Vertex { float pos[3]; float normal[3]; uint32_t meshlet_id; };
    std::vector<Vertex> verts(vertex_count);

    for (size_t i = 0; i < vertex_count; i++) {
        verts[i].pos[0] = asset.positions[i * 3 + 0];
        verts[i].pos[1] = asset.positions[i * 3 + 1];
        verts[i].pos[2] = asset.positions[i * 3 + 2];

        if (i < asset.normals.size()) {
            float nx = static_cast<float>(asset.normals[i].x) / 32767.0f;
            float ny = static_cast<float>(asset.normals[i].y) / 32767.0f;
            float nz = 1.0f - std::abs(nx) - std::abs(ny);
            if (nz < 0) {
                float tx = nx;
                nx = (1.0f - std::abs(ny)) * (nx >= 0 ? 1.0f : -1.0f);
                ny = (1.0f - std::abs(tx)) * (ny >= 0 ? 1.0f : -1.0f);
            }
            float len = std::sqrt(nx*nx + ny*ny + nz*nz);
            verts[i].normal[0] = nx / len;
            verts[i].normal[1] = ny / len;
            verts[i].normal[2] = nz / len;
        } else {
            verts[i].normal[0] = 0.0f;
            verts[i].normal[1] = 1.0f;
            verts[i].normal[2] = 0.0f;
        }

        verts[i].meshlet_id = vertex_meshlet_ids[i];
    }

    // Create vertex buffer
    VkDeviceSize vertex_size = verts.size() * sizeof(Vertex);

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = vertex_size;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    vkCreateBuffer(m_device, &buffer_info, nullptr, &m_vertex_buffer);

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(m_device, m_vertex_buffer, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(m_device, &alloc_info, nullptr, &m_vertex_memory);
    vkBindBufferMemory(m_device, m_vertex_buffer, m_vertex_memory, 0);

    void* data;
    vkMapMemory(m_device, m_vertex_memory, 0, vertex_size, 0, &data);
    memcpy(data, verts.data(), vertex_size);
    vkUnmapMemory(m_device, m_vertex_memory);

    m_vertex_count = static_cast<uint32_t>(vertex_count);

    // Create index buffer
    if (!asset.indices.empty()) {
        VkDeviceSize index_size = asset.indices.size() * sizeof(uint32_t);

        buffer_info.size = index_size;
        buffer_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

        vkCreateBuffer(m_device, &buffer_info, nullptr, &m_index_buffer);
        vkGetBufferMemoryRequirements(m_device, m_index_buffer, &mem_reqs);

        alloc_info.allocationSize = mem_reqs.size;
        alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(m_device, &alloc_info, nullptr, &m_index_memory);
        vkBindBufferMemory(m_device, m_index_buffer, m_index_memory, 0);

        vkMapMemory(m_device, m_index_memory, 0, index_size, 0, &data);
        memcpy(data, asset.indices.data(), index_size);
        vkUnmapMemory(m_device, m_index_memory);

        m_index_count = static_cast<uint32_t>(asset.indices.size());
    }

    m_has_asset = true;
}

void OffscreenRenderer::render(const Camera& camera, const ClusterManager& clusters, const float* model_matrix) {
    if (!m_initialized) return;

    vkWaitForFences(m_device, 1, &m_render_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &m_render_fence);

    vkResetCommandBuffer(m_command_buffer, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(m_command_buffer, &begin_info);

    VkRenderPassBeginInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_info.renderPass = m_render_pass;
    rp_info.framebuffer = m_framebuffer;
    rp_info.renderArea.offset = {0, 0};
    rp_info.renderArea.extent = {m_width, m_height};

    std::array<VkClearValue, 2> clear_values{};
    clear_values[0].color = {{0.1f, 0.1f, 0.15f, 1.0f}};
    clear_values[1].depthStencil = {1.0f, 0};
    rp_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
    rp_info.pClearValues = clear_values.data();

    vkCmdBeginRenderPass(m_command_buffer, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkViewport viewport{};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_command_buffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(m_command_buffer, 0, 1, &scissor);

    struct PushConstants {
        float mvp[16];
        float model[16];
        uint32_t debug_mode;
    } push_data;

    // Identity matrix for when no model transform
    static const float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    const float* model = model_matrix ? model_matrix : identity;

    // Compute MVP = ViewProjection * Model
    if (model_matrix) {
        for (int col = 0; col < 4; col++) {
            for (int row = 0; row < 4; row++) {
                push_data.mvp[col * 4 + row] = 0.0f;
                for (int k = 0; k < 4; k++) {
                    push_data.mvp[col * 4 + row] += camera.view_projection[k * 4 + row] * model_matrix[col * 4 + k];
                }
            }
        }
    } else {
        std::memcpy(push_data.mvp, camera.view_projection, sizeof(push_data.mvp));
    }

    std::memcpy(push_data.model, model, sizeof(push_data.model));
    push_data.debug_mode = m_debug_mode ? 1u : 0u;

    vkCmdPushConstants(m_command_buffer, m_pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push_data), &push_data);

    if (m_has_asset && m_vertex_buffer) {
        VkBuffer buffers[] = {m_vertex_buffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(m_command_buffer, 0, 1, buffers, offsets);

        if (m_index_buffer) {
            vkCmdBindIndexBuffer(m_command_buffer, m_index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(m_command_buffer, m_index_count, 1, 0, 0, 0);
        } else {
            vkCmdDraw(m_command_buffer, m_vertex_count, 1, 0, 0);
        }
    }

    vkCmdEndRenderPass(m_command_buffer);

    vkEndCommandBuffer(m_command_buffer);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_command_buffer;

    vkQueueSubmit(m_graphics_queue, 1, &submit, m_render_fence);
}

SharedImageInfo OffscreenRenderer::get_shared_image_info() const {
    SharedImageInfo info{};
    info.handle = m_color_handle;
    info.width = m_width;
    info.height = m_height;
    info.valid = (m_color_handle != INVALID_SHARED_MEMORY_HANDLE);

    // Get allocation size
    if (m_device != VK_NULL_HANDLE && m_color_image != VK_NULL_HANDLE) {
        VkMemoryRequirements mem_reqs;
        vkGetImageMemoryRequirements(m_device, m_color_image, &mem_reqs);
        info.allocation_size = mem_reqs.size;
    }

    return info;
}

bool OffscreenRenderer::read_pixels(uint8_t* out_pixels) {
    if (!m_initialized || !out_pixels) return false;

    // Wait for render to complete
    vkWaitForFences(m_device, 1, &m_render_fence, VK_TRUE, UINT64_MAX);

    // Copy image to staging buffer
    vkResetCommandBuffer(m_command_buffer, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_command_buffer, &begin_info);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {m_width, m_height, 1};

    vkCmdCopyImageToBuffer(m_command_buffer, m_color_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_staging_buffer, 1, &region);

    vkEndCommandBuffer(m_command_buffer);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_command_buffer;

    vkResetFences(m_device, 1, &m_render_fence);
    vkQueueSubmit(m_graphics_queue, 1, &submit, m_render_fence);
    vkWaitForFences(m_device, 1, &m_render_fence, VK_TRUE, UINT64_MAX);

    // Read from staging buffer
    void* data;
    vkMapMemory(m_device, m_staging_memory, 0, m_width * m_height * 4, 0, &data);
    std::memcpy(out_pixels, data, m_width * m_height * 4);
    vkUnmapMemory(m_device, m_staging_memory);

    return true;
}

} // namespace vgeo
