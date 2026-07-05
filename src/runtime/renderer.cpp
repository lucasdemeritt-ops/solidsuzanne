// VGEO Renderer
// Vulkan-based cluster renderer with indirect drawing

#include "renderer.h"
#include "vgeo_loader.h"
#include "cluster_manager.h"
#include "camera.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <iostream>
#include <fstream>
#include <cstring>
#include <cmath>
#include <set>
#include <algorithm>
#include <array>
#include <filesystem>

namespace vgeo {

// Helper function to find shader directory
static std::string find_shader_dir() {
    // Try common locations relative to executable
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
        if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
            return path;
        }
    }

    return "shaders";  // Default
}

// Load shader from file
static std::vector<uint32_t> load_shader_file(const std::string& filename) {
    std::string shader_dir = find_shader_dir();
    std::string full_path = shader_dir + "/" + filename;

    std::cout << "    Loading shader: " << full_path << "\n" << std::flush;

    std::ifstream file(full_path, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        std::cerr << "Failed to open shader file: " << full_path << "\n";
        return {};
    }

    size_t file_size = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> buffer(file_size / sizeof(uint32_t));

    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    file.close();

    return buffer;
}

// Fallback embedded SPIR-V shaders (minimal, for testing only)
// These are minimal shaders for testing - will be replaced with file loading

// Vertex shader for basic rendering
static const uint32_t basic_vert_spv[] = {
    // #version 450
    // layout(location = 0) in vec3 inPosition;
    // layout(location = 1) in vec3 inNormal;
    // layout(push_constant) uniform PC { mat4 mvp; } pc;
    // layout(location = 0) out vec3 fragNormal;
    // void main() { gl_Position = pc.mvp * vec4(inPosition, 1.0); fragNormal = inNormal; }
    0x07230203, 0x00010000, 0x000d000a, 0x00000030,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x000a000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000d, 0x00000012, 0x0000001c,
    0x00000020, 0x00000029, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00060005, 0x0000000b, 0x505f6c67,
    0x65567265, 0x78657472, 0x00000000, 0x00060006,
    0x0000000b, 0x00000000, 0x505f6c67, 0x7469736f,
    0x006e6f69, 0x00030005, 0x0000000d, 0x00000000,
    0x00030005, 0x0000000f, 0x00004350, 0x00040006,
    0x0000000f, 0x00000000, 0x0070766d, 0x00030005,
    0x00000011, 0x00006370, 0x00050005, 0x00000012,
    0x6f506e69, 0x69746973, 0x00006e6f, 0x00050005,
    0x0000001c, 0x67617266, 0x6d726f4e, 0x00006c61,
    0x00050005, 0x00000020, 0x6f4e6e69, 0x6c616d72,
    0x00000000, 0x00050005, 0x00000029, 0x67617266,
    0x6f6c6f43, 0x00000072, 0x00050048, 0x0000000b,
    0x00000000, 0x0000000b, 0x00000000, 0x00030047,
    0x0000000b, 0x00000002, 0x00040048, 0x0000000f,
    0x00000000, 0x00000005, 0x00050048, 0x0000000f,
    0x00000000, 0x00000023, 0x00000000, 0x00050048,
    0x0000000f, 0x00000000, 0x00000007, 0x00000010,
    0x00030047, 0x0000000f, 0x00000002, 0x00040047,
    0x00000012, 0x0000001e, 0x00000000, 0x00040047,
    0x0000001c, 0x0000001e, 0x00000000, 0x00040047,
    0x00000020, 0x0000001e, 0x00000001, 0x00040047,
    0x00000029, 0x0000001e, 0x00000001, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017,
    0x00000007, 0x00000006, 0x00000004, 0x0003001e,
    0x0000000b, 0x00000007, 0x00040020, 0x0000000c,
    0x00000003, 0x0000000b, 0x0004003b, 0x0000000c,
    0x0000000d, 0x00000003, 0x00040015, 0x0000000e,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000e,
    0x00000010, 0x00000000, 0x00040018, 0x00000013,
    0x00000007, 0x00000004, 0x0003001e, 0x0000000f,
    0x00000013, 0x00040020, 0x00000014, 0x00000009,
    0x0000000f, 0x0004003b, 0x00000014, 0x00000011,
    0x00000009, 0x00040020, 0x00000015, 0x00000009,
    0x00000013, 0x00040017, 0x00000018, 0x00000006,
    0x00000003, 0x00040020, 0x00000019, 0x00000001,
    0x00000018, 0x0004003b, 0x00000019, 0x00000012,
    0x00000001, 0x0004002b, 0x00000006, 0x0000001a,
    0x3f800000, 0x00040020, 0x0000001b, 0x00000003,
    0x00000018, 0x0004003b, 0x0000001b, 0x0000001c,
    0x00000003, 0x0004003b, 0x00000019, 0x00000020,
    0x00000001, 0x00040020, 0x00000028, 0x00000003,
    0x00000007, 0x0004003b, 0x00000028, 0x00000029,
    0x00000003, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x00050041, 0x00000015, 0x00000016, 0x00000011,
    0x00000010, 0x0004003d, 0x00000013, 0x00000017,
    0x00000016, 0x0004003d, 0x00000018, 0x0000001d,
    0x00000012, 0x00050051, 0x00000006, 0x0000001e,
    0x0000001d, 0x00000000, 0x00050051, 0x00000006,
    0x0000001f, 0x0000001d, 0x00000001, 0x00050051,
    0x00000006, 0x00000021, 0x0000001d, 0x00000002,
    0x00070050, 0x00000007, 0x00000022, 0x0000001e,
    0x0000001f, 0x00000021, 0x0000001a, 0x00050091,
    0x00000007, 0x00000023, 0x00000017, 0x00000022,
    0x00050041, 0x00000028, 0x00000024, 0x0000000d,
    0x00000010, 0x0003003e, 0x00000024, 0x00000023,
    0x0004003d, 0x00000018, 0x00000025, 0x00000020,
    0x0003003e, 0x0000001c, 0x00000025, 0x0004003d,
    0x00000018, 0x0000002a, 0x00000020, 0x00050051,
    0x00000006, 0x0000002b, 0x0000002a, 0x00000000,
    0x00050051, 0x00000006, 0x0000002c, 0x0000002a,
    0x00000001, 0x00050051, 0x00000006, 0x0000002d,
    0x0000002a, 0x00000002, 0x00070050, 0x00000007,
    0x0000002e, 0x0000002b, 0x0000002c, 0x0000002d,
    0x0000001a, 0x0003003e, 0x00000029, 0x0000002e,
    0x000100fd, 0x00010038
};

// Fragment shader for basic rendering
static const uint32_t basic_frag_spv[] = {
    // #version 450
    // layout(location = 0) in vec3 fragNormal;
    // layout(location = 0) out vec4 outColor;
    // void main() {
    //     vec3 N = normalize(fragNormal);
    //     float NdotL = max(dot(N, normalize(vec3(1,1,1))), 0.0);
    //     outColor = vec4(vec3(0.5) + vec3(0.5) * NdotL, 1.0);
    // }
    0x07230203, 0x00010000, 0x000d000a, 0x00000024,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d,
    0x00000000, 0x00000009, 0x00000011, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00040005, 0x00000009, 0x4374756f,
    0x726f6c6f, 0x00000000, 0x00050005, 0x00000011,
    0x67617266, 0x6d726f4e, 0x00006c61, 0x00040047,
    0x00000009, 0x0000001e, 0x00000000, 0x00040047,
    0x00000011, 0x0000001e, 0x00000000, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017,
    0x00000007, 0x00000006, 0x00000004, 0x00040020,
    0x00000008, 0x00000003, 0x00000007, 0x0004003b,
    0x00000008, 0x00000009, 0x00000003, 0x00040017,
    0x0000000a, 0x00000006, 0x00000003, 0x00040020,
    0x00000010, 0x00000001, 0x0000000a, 0x0004003b,
    0x00000010, 0x00000011, 0x00000001, 0x0004002b,
    0x00000006, 0x00000014, 0x3f3504f3, 0x0006002c,
    0x0000000a, 0x00000015, 0x00000014, 0x00000014,
    0x00000014, 0x0004002b, 0x00000006, 0x00000017,
    0x00000000, 0x0004002b, 0x00000006, 0x0000001a,
    0x3f000000, 0x0006002c, 0x0000000a, 0x0000001b,
    0x0000001a, 0x0000001a, 0x0000001a, 0x0004002b,
    0x00000006, 0x00000021, 0x3f800000, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x0004003d, 0x0000000a,
    0x00000012, 0x00000011, 0x0006000c, 0x0000000a,
    0x00000013, 0x00000001, 0x00000045, 0x00000012,
    0x00050094, 0x00000006, 0x00000016, 0x00000013,
    0x00000015, 0x0007000c, 0x00000006, 0x00000018,
    0x00000001, 0x00000028, 0x00000016, 0x00000017,
    0x0005008e, 0x0000000a, 0x0000001c, 0x0000001b,
    0x00000018, 0x00050081, 0x0000000a, 0x0000001d,
    0x0000001b, 0x0000001c, 0x00050051, 0x00000006,
    0x0000001e, 0x0000001d, 0x00000000, 0x00050051,
    0x00000006, 0x0000001f, 0x0000001d, 0x00000001,
    0x00050051, 0x00000006, 0x00000020, 0x0000001d,
    0x00000002, 0x00070050, 0x00000007, 0x00000022,
    0x0000001e, 0x0000001f, 0x00000020, 0x00000021,
    0x0003003e, 0x00000009, 0x00000022, 0x000100fd,
    0x00010038
};

// Validation layer callback
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void* user_data) {
    (void)type;
    (void)user_data;

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << "Vulkan validation: " << callback_data->pMessage << std::endl;
    }
    return VK_FALSE;
}

bool Renderer::init(void* window_handle, uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    std::cout << "  Creating instance...\n" << std::flush;
    if (!create_instance()) return false;
    std::cout << "  Creating surface...\n" << std::flush;
    if (!create_surface(window_handle)) return false;
    std::cout << "  Selecting physical device...\n" << std::flush;
    if (!select_physical_device()) return false;
    std::cout << "  Creating logical device...\n" << std::flush;
    if (!create_device()) return false;
    std::cout << "  Creating swapchain...\n" << std::flush;
    if (!create_swapchain()) return false;
    std::cout << "  Creating render pass...\n" << std::flush;
    if (!create_render_pass()) return false;
    std::cout << "  Creating framebuffers...\n" << std::flush;
    if (!create_framebuffers()) return false;
    std::cout << "  Creating command pool...\n" << std::flush;
    if (!create_command_pool()) return false;
    std::cout << "  Creating sync objects...\n" << std::flush;
    if (!create_sync_objects()) return false;
    std::cout << "  Creating pipeline...\n" << std::flush;
    if (!create_pipeline()) return false;
    std::cout << "  Creating triangle buffers...\n" << std::flush;
    if (!create_triangle_buffers()) return false;

    std::cout << "Vulkan renderer initialized successfully\n";
    return true;
}

void Renderer::destroy() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }

    // Cleanup triangle buffers (handles are nulled so destroy() is
    // idempotent — a second call must not double-free)
    if (m_triangle_vertex_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_triangle_vertex_buffer, nullptr);
        vkFreeMemory(m_device, m_triangle_vertex_memory, nullptr);
        m_triangle_vertex_buffer = VK_NULL_HANDLE;
        m_triangle_vertex_memory = VK_NULL_HANDLE;
    }

    // Cleanup geometry buffers
    if (m_vertex_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_vertex_buffer, nullptr);
        vkFreeMemory(m_device, m_vertex_memory, nullptr);
        m_vertex_buffer = VK_NULL_HANDLE;
        m_vertex_memory = VK_NULL_HANDLE;
    }
    if (m_index_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_index_buffer, nullptr);
        vkFreeMemory(m_device, m_index_memory, nullptr);
        m_index_buffer = VK_NULL_HANDLE;
        m_index_memory = VK_NULL_HANDLE;
    }

    // Cleanup pipeline
    if (m_render_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_render_pipeline, nullptr);
        m_render_pipeline = VK_NULL_HANDLE;
    }
    if (m_render_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_render_layout, nullptr);
        m_render_layout = VK_NULL_HANDLE;
    }

    // Cleanup sync objects (render-finished semaphores are per swapchain
    // image, so iterate the full vectors, not MAX_FRAMES_IN_FLIGHT)
    for (auto semaphore : m_render_finished_semaphores) {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_device, semaphore, nullptr);
    }
    m_render_finished_semaphores.clear();
    for (auto semaphore : m_image_available_semaphores) {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_device, semaphore, nullptr);
    }
    m_image_available_semaphores.clear();
    for (auto fence : m_in_flight_fences) {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(m_device, fence, nullptr);
    }
    m_in_flight_fences.clear();

    // Cleanup command pool
    if (m_command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_command_pool, nullptr);
        m_command_pool = VK_NULL_HANDLE;
    }

    cleanup_swapchain();

    // Cleanup render pass
    if (m_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_render_pass, nullptr);
        m_render_pass = VK_NULL_HANDLE;
    }

    // Cleanup device
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    // Cleanup debug messenger (needs a live instance)
    if (ENABLE_VALIDATION && m_debug_messenger != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func) {
            func(m_instance, m_debug_messenger, nullptr);
        }
        m_debug_messenger = VK_NULL_HANDLE;
    }

    // Cleanup surface and instance
    if (m_surface != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

bool Renderer::create_instance() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "VGEO Viewer";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "VGEO";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    // Get required extensions from GLFW
    uint32_t glfw_ext_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_ext_count);

    std::vector<const char*> extensions(glfw_extensions, glfw_extensions + glfw_ext_count);

    // Add debug extension if validation is enabled
    if (ENABLE_VALIDATION) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    // Validation layers
    const char* validation_layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkDebugUtilsMessengerCreateInfoEXT debug_create_info{};

    if (ENABLE_VALIDATION) {
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = validation_layers;

        debug_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debug_create_info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_create_info.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug_create_info.pfnUserCallback = debug_callback;

        create_info.pNext = &debug_create_info;
    } else {
        create_info.enabledLayerCount = 0;
    }

    if (vkCreateInstance(&create_info, nullptr, &m_instance) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance\n";
        return false;
    }

    // Create debug messenger
    if (ENABLE_VALIDATION) {
        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (func) {
            func(m_instance, &debug_create_info, nullptr, &m_debug_messenger);
        }
    }

    return true;
}

bool Renderer::create_surface(void* window_handle) {
    GLFWwindow* window = static_cast<GLFWwindow*>(window_handle);
    if (glfwCreateWindowSurface(m_instance, window, nullptr, &m_surface) != VK_SUCCESS) {
        std::cerr << "Failed to create window surface\n";
        return false;
    }
    return true;
}

bool Renderer::select_physical_device() {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr);

    if (device_count == 0) {
        std::cerr << "No Vulkan-capable GPU found\n";
        return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data());

    // Find a suitable device
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        // Find queue families
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

        bool has_graphics = false;
        bool has_present = false;

        for (uint32_t i = 0; i < queue_family_count; i++) {
            if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                m_graphics_family = i;
                has_graphics = true;
            }

            VkBool32 present_support = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present_support);
            if (present_support) {
                m_present_family = i;
                has_present = true;
            }

            if (has_graphics && has_present) break;
        }

        // Check for swapchain extension
        uint32_t ext_count;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> available_extensions(ext_count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, available_extensions.data());

        bool has_swapchain = false;
        for (const auto& ext : available_extensions) {
            if (strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                has_swapchain = true;
                break;
            }
        }

        if (has_graphics && has_present && has_swapchain) {
            m_physical_device = device;
            std::cout << "Selected GPU: " << props.deviceName << "\n";
            return true;
        }
    }

    std::cerr << "No suitable GPU found\n";
    return false;
}

bool Renderer::create_device() {
    std::set<uint32_t> unique_families = {m_graphics_family, m_present_family};

    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
    float queue_priority = 1.0f;

    for (uint32_t family : unique_families) {
        VkDeviceQueueCreateInfo queue_create_info{};
        queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_info.queueFamilyIndex = family;
        queue_create_info.queueCount = 1;
        queue_create_info.pQueuePriorities = &queue_priority;
        queue_create_infos.push_back(queue_create_info);
    }

    VkPhysicalDeviceFeatures device_features{};
    device_features.fillModeNonSolid = VK_TRUE;  // For wireframe

    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
    create_info.pQueueCreateInfos = queue_create_infos.data();
    create_info.pEnabledFeatures = &device_features;
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = device_extensions;

    if (vkCreateDevice(m_physical_device, &create_info, nullptr, &m_device) != VK_SUCCESS) {
        std::cerr << "Failed to create logical device\n";
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphics_family, 0, &m_graphics_queue);
    vkGetDeviceQueue(m_device, m_present_family, 0, &m_present_queue);

    return true;
}

bool Renderer::create_swapchain() {
    // Query surface capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device, m_surface, &capabilities);

    // A minimized window reports a 0x0 extent; creating a zero-extent
    // swapchain is invalid. Keep the old swapchain and try again later.
    if (capabilities.currentExtent.width == 0 || capabilities.currentExtent.height == 0) {
        return false;
    }

    // Choose format
    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, formats.data());

    VkSurfaceFormatKHR surface_format = formats[0];
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = format;
            break;
        }
    }
    m_swapchain_format = surface_format.format;

    // Choose present mode
    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical_device, m_surface, &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical_device, m_surface, &present_mode_count, present_modes.data());

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;  // Always available
    for (const auto& mode : present_modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = mode;
            break;
        }
    }

    // Choose extent
    if (capabilities.currentExtent.width != UINT32_MAX) {
        m_swapchain_extent = capabilities.currentExtent;
    } else {
        m_swapchain_extent.width = std::clamp(m_width,
            capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        m_swapchain_extent.height = std::clamp(m_height,
            capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = m_surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = m_swapchain_extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queue_family_indices[] = {m_graphics_family, m_present_family};
    if (m_graphics_family != m_present_family) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_family_indices;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(m_device, &create_info, nullptr, &m_swapchain) != VK_SUCCESS) {
        std::cerr << "Failed to create swapchain\n";
        return false;
    }

    // Get swapchain images
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, nullptr);
    m_swapchain_images.resize(image_count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, m_swapchain_images.data());

    // Create image views
    m_swapchain_image_views.resize(image_count);
    for (size_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = m_swapchain_images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = m_swapchain_format;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &view_info, nullptr, &m_swapchain_image_views[i]) != VK_SUCCESS) {
            std::cerr << "Failed to create image view\n";
            return false;
        }
    }

    // Create depth buffer
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

    VkImageCreateInfo depth_image_info{};
    depth_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_image_info.imageType = VK_IMAGE_TYPE_2D;
    depth_image_info.extent.width = m_swapchain_extent.width;
    depth_image_info.extent.height = m_swapchain_extent.height;
    depth_image_info.extent.depth = 1;
    depth_image_info.mipLevels = 1;
    depth_image_info.arrayLayers = 1;
    depth_image_info.format = depth_format;
    depth_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth_image_info.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(m_device, &depth_image_info, nullptr, &m_depth_image) != VK_SUCCESS) {
        std::cerr << "Failed to create depth image\n";
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(m_device, m_depth_image, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &alloc_info, nullptr, &m_depth_memory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate depth image memory\n";
        return false;
    }

    vkBindImageMemory(m_device, m_depth_image, m_depth_memory, 0);

    VkImageViewCreateInfo depth_view_info{};
    depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_info.image = m_depth_image;
    depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_info.format = depth_format;
    depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_view_info.subresourceRange.baseMipLevel = 0;
    depth_view_info.subresourceRange.levelCount = 1;
    depth_view_info.subresourceRange.baseArrayLayer = 0;
    depth_view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &depth_view_info, nullptr, &m_depth_view) != VK_SUCCESS) {
        std::cerr << "Failed to create depth image view\n";
        return false;
    }

    return true;
}

bool Renderer::create_render_pass() {
    VkAttachmentDescription color_attachment{};
    color_attachment.format = m_swapchain_format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth_attachment{};
    depth_attachment.format = VK_FORMAT_D32_SFLOAT;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_attachment_ref{};
    color_attachment_ref.attachment = 0;
    color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_attachment_ref{};
    depth_attachment_ref.attachment = 1;
    depth_attachment_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment_ref;
    subpass.pDepthStencilAttachment = &depth_attachment_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {color_attachment, depth_attachment};

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = static_cast<uint32_t>(attachments.size());
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;

    if (vkCreateRenderPass(m_device, &render_pass_info, nullptr, &m_render_pass) != VK_SUCCESS) {
        std::cerr << "Failed to create render pass\n";
        return false;
    }

    return true;
}

bool Renderer::create_framebuffers() {
    m_framebuffers.resize(m_swapchain_image_views.size());

    for (size_t i = 0; i < m_swapchain_image_views.size(); i++) {
        std::array<VkImageView, 2> attachments = {
            m_swapchain_image_views[i],
            m_depth_view
        };

        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = m_render_pass;
        framebuffer_info.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebuffer_info.pAttachments = attachments.data();
        framebuffer_info.width = m_swapchain_extent.width;
        framebuffer_info.height = m_swapchain_extent.height;
        framebuffer_info.layers = 1;

        if (vkCreateFramebuffer(m_device, &framebuffer_info, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            std::cerr << "Failed to create framebuffer\n";
            return false;
        }
    }

    return true;
}

bool Renderer::create_command_pool() {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = m_graphics_family;

    if (vkCreateCommandPool(m_device, &pool_info, nullptr, &m_command_pool) != VK_SUCCESS) {
        std::cerr << "Failed to create command pool\n";
        return false;
    }

    // Allocate command buffers
    m_command_buffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = m_command_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(m_command_buffers.size());

    if (vkAllocateCommandBuffers(m_device, &alloc_info, m_command_buffers.data()) != VK_SUCCESS) {
        std::cerr << "Failed to allocate command buffers\n";
        return false;
    }

    return true;
}

bool Renderer::create_sync_objects() {
    m_image_available_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
    // render-finished semaphores are per swapchain image, not per frame in
    // flight: the presentation engine may still hold a pending wait on the
    // semaphore when a per-frame slot is reused (typically 3+ images vs 2
    // frames in flight)
    m_render_finished_semaphores.resize(m_swapchain_images.size());
    m_in_flight_fences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(m_device, &semaphore_info, nullptr, &m_image_available_semaphores[i]) != VK_SUCCESS ||
            vkCreateFence(m_device, &fence_info, nullptr, &m_in_flight_fences[i]) != VK_SUCCESS) {
            std::cerr << "Failed to create sync objects\n";
            return false;
        }
    }

    for (auto& semaphore : m_render_finished_semaphores) {
        if (vkCreateSemaphore(m_device, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
            std::cerr << "Failed to create sync objects\n";
            return false;
        }
    }

    return true;
}

VkShaderModule Renderer::create_shader_module(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = size;
    create_info.pCode = code;

    VkShaderModule shader_module;
    if (vkCreateShaderModule(m_device, &create_info, nullptr, &shader_module) != VK_SUCCESS) {
        std::cerr << "Failed to create shader module\n";
        return VK_NULL_HANDLE;
    }
    return shader_module;
}

bool Renderer::create_pipeline() {
    // Try to load shaders from files first
    std::cout << "    Loading shaders from files...\n" << std::flush;

    std::vector<uint32_t> vert_code = load_shader_file("basic.vert.spv");
    std::vector<uint32_t> frag_code = load_shader_file("basic.frag.spv");

    VkShaderModule vert_shader = VK_NULL_HANDLE;
    VkShaderModule frag_shader = VK_NULL_HANDLE;

    if (!vert_code.empty() && !frag_code.empty()) {
        std::cout << "    Creating vertex shader module from file...\n" << std::flush;
        vert_shader = create_shader_module(vert_code.data(), vert_code.size() * sizeof(uint32_t));
        std::cout << "    Creating fragment shader module from file...\n" << std::flush;
        frag_shader = create_shader_module(frag_code.data(), frag_code.size() * sizeof(uint32_t));
    }

    // Fallback to embedded shaders if file loading failed
    if (vert_shader == VK_NULL_HANDLE) {
        std::cout << "    Falling back to embedded vertex shader...\n" << std::flush;
        vert_shader = create_shader_module(basic_vert_spv, sizeof(basic_vert_spv));
    }
    if (frag_shader == VK_NULL_HANDLE) {
        std::cout << "    Falling back to embedded fragment shader...\n" << std::flush;
        frag_shader = create_shader_module(basic_frag_spv, sizeof(basic_frag_spv));
    }

    if (vert_shader == VK_NULL_HANDLE || frag_shader == VK_NULL_HANDLE) {
        std::cerr << "    Shader module creation failed!\n" << std::flush;
        return false;
    }
    std::cout << "    Shader modules created successfully\n" << std::flush;

    VkPipelineShaderStageCreateInfo vert_stage_info{};
    vert_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage_info.module = vert_shader;
    vert_stage_info.pName = "main";

    VkPipelineShaderStageCreateInfo frag_stage_info{};
    frag_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage_info.module = frag_shader;
    frag_stage_info.pName = "main";

    VkPipelineShaderStageCreateInfo shader_stages[] = {vert_stage_info, frag_stage_info};

    // Vertex input (position + normal)
    VkVertexInputBindingDescription binding_desc{};
    binding_desc.binding = 0;
    binding_desc.stride = sizeof(float) * 6;  // pos (3) + normal (3)
    binding_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attr_descs{};
    attr_descs[0].binding = 0;
    attr_descs[0].location = 0;
    attr_descs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attr_descs[0].offset = 0;

    attr_descs[1].binding = 0;
    attr_descs[1].location = 1;
    attr_descs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attr_descs[1].offset = sizeof(float) * 3;

    VkPipelineVertexInputStateCreateInfo vertex_input_info{};
    vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_info.vertexBindingDescriptionCount = 1;
    vertex_input_info.pVertexBindingDescriptions = &binding_desc;
    vertex_input_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attr_descs.size());
    vertex_input_info.pVertexAttributeDescriptions = attr_descs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_swapchain_extent.width);
    viewport.height = static_cast<float>(m_swapchain_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_swapchain_extent;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &color_blend_attachment;

    // Dynamic state for viewport and scissor
    std::array<VkDynamicState, 2> dynamic_states = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    // Push constants for MVP and Model matrices
    VkPushConstantRange push_constant{};
    push_constant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_constant.offset = 0;
    push_constant.size = sizeof(float) * 32;  // 2x mat4 (MVP + Model)

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constant;

    std::cout << "    Creating pipeline layout...\n" << std::flush;
    if (vkCreatePipelineLayout(m_device, &layout_info, nullptr, &m_render_layout) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout\n";
        vkDestroyShaderModule(m_device, vert_shader, nullptr);
        vkDestroyShaderModule(m_device, frag_shader, nullptr);
        return false;
    }
    std::cout << "    Pipeline layout created\n" << std::flush;

    std::cout << "    Setting up graphics pipeline...\n" << std::flush;
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input_info;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = m_render_layout;
    pipeline_info.renderPass = m_render_pass;
    pipeline_info.subpass = 0;

    std::cout << "    Calling vkCreateGraphicsPipelines...\n" << std::flush;
    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_render_pipeline);
    std::cout << "    vkCreateGraphicsPipelines returned: " << result << "\n" << std::flush;

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create graphics pipeline (error: " << result << ")\n";
        vkDestroyShaderModule(m_device, vert_shader, nullptr);
        vkDestroyShaderModule(m_device, frag_shader, nullptr);
        return false;
    }

    std::cout << "    Cleaning up shader modules...\n" << std::flush;
    vkDestroyShaderModule(m_device, vert_shader, nullptr);
    vkDestroyShaderModule(m_device, frag_shader, nullptr);

    std::cout << "    Pipeline created successfully!\n" << std::flush;
    return true;
}

bool Renderer::create_triangle_buffers() {
    // Simple triangle with positions and normals
    // Format: x, y, z, nx, ny, nz
    float vertices[] = {
        // Position          // Normal
        -0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f,
         0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f,
         0.0f,  0.5f, 0.0f,  0.0f, 0.0f, 1.0f,
    };

    VkDeviceSize buffer_size = sizeof(vertices);

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &buffer_info, nullptr, &m_triangle_vertex_buffer) != VK_SUCCESS) {
        std::cerr << "Failed to create vertex buffer\n";
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(m_device, m_triangle_vertex_buffer, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &alloc_info, nullptr, &m_triangle_vertex_memory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate vertex buffer memory\n";
        return false;
    }

    vkBindBufferMemory(m_device, m_triangle_vertex_buffer, m_triangle_vertex_memory, 0);

    void* data;
    vkMapMemory(m_device, m_triangle_vertex_memory, 0, buffer_size, 0, &data);
    memcpy(data, vertices, buffer_size);
    vkUnmapMemory(m_device, m_triangle_vertex_memory);

    return true;
}

uint32_t Renderer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    std::cerr << "Failed to find suitable memory type\n";
    return 0;
}

void Renderer::cleanup_swapchain() {
    // Cleanup depth buffer
    if (m_depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_depth_view, nullptr);
        m_depth_view = VK_NULL_HANDLE;
    }
    if (m_depth_image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_depth_image, nullptr);
        m_depth_image = VK_NULL_HANDLE;
    }
    if (m_depth_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_depth_memory, nullptr);
        m_depth_memory = VK_NULL_HANDLE;
    }

    // Cleanup framebuffers
    for (auto framebuffer : m_framebuffers) {
        vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    }
    m_framebuffers.clear();

    // Cleanup image views
    for (auto image_view : m_swapchain_image_views) {
        vkDestroyImageView(m_device, image_view, nullptr);
    }
    m_swapchain_image_views.clear();

    // Cleanup swapchain
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

void Renderer::recreate_swapchain() {
    vkDeviceWaitIdle(m_device);

    cleanup_swapchain();

    if (!create_swapchain()) {
        // Window is minimized (0x0) or creation failed; keep the resized
        // flag set so the next frame retries instead of drawing into
        // destroyed framebuffers
        m_framebuffer_resized = true;
        return;
    }
    create_framebuffers();

    // render-finished semaphores are per swapchain image; recreate them if
    // the image count changed (safe: device is idle at this point)
    if (m_render_finished_semaphores.size() != m_swapchain_images.size()) {
        for (auto semaphore : m_render_finished_semaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_device, semaphore, nullptr);
            }
        }
        m_render_finished_semaphores.assign(m_swapchain_images.size(), VK_NULL_HANDLE);

        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto& semaphore : m_render_finished_semaphores) {
            vkCreateSemaphore(m_device, &semaphore_info, nullptr, &semaphore);
        }
    }
}

void Renderer::resize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    m_framebuffer_resized = true;
}

void Renderer::render_triangle(const Camera& camera) {
    // Wait for previous frame
    // The swapchain can be gone after a failed recreation (e.g. minimized
    // window); retry creating it before touching any frame resources
    if (m_swapchain == VK_NULL_HANDLE) {
        recreate_swapchain();
        if (m_swapchain == VK_NULL_HANDLE) {
            return;
        }
    }

    // Bounded wait: if a previous submit failed the fence never signals,
    // and an infinite wait would hang the application forever. On timeout
    // skip the frame — the command buffer may still be pending.
    if (vkWaitForFences(m_device, 1, &m_in_flight_fences[m_current_frame], VK_TRUE,
                        2'000'000'000ull) != VK_SUCCESS) {
        std::cerr << "Timed out waiting for frame fence; skipping frame\n";
        return;
    }

    // Acquire next image
    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
        m_image_available_semaphores[m_current_frame], VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        std::cerr << "Failed to acquire swapchain image\n";
        return;
    }

    vkResetFences(m_device, 1, &m_in_flight_fences[m_current_frame]);

    // Record command buffer
    VkCommandBuffer cmd = m_command_buffers[m_current_frame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    vkBeginCommandBuffer(cmd, &begin_info);

    // Begin render pass
    VkRenderPassBeginInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = m_render_pass;
    render_pass_info.framebuffer = m_framebuffers[image_index];
    render_pass_info.renderArea.offset = {0, 0};
    render_pass_info.renderArea.extent = m_swapchain_extent;

    std::array<VkClearValue, 2> clear_values{};
    clear_values[0].color = {{0.1f, 0.1f, 0.15f, 1.0f}};
    clear_values[1].depthStencil = {1.0f, 0};

    render_pass_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
    render_pass_info.pClearValues = clear_values.data();

    vkCmdBeginRenderPass(cmd, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_render_pipeline);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_swapchain_extent.width);
    viewport.height = static_cast<float>(m_swapchain_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_swapchain_extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Push MVP and Model matrices (identity model for test triangle)
    struct {
        float mvp[16];
        float model[16];
    } push_data;
    std::memcpy(push_data.mvp, camera.view_projection, sizeof(push_data.mvp));
    // Identity model matrix
    float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(push_data.model, identity, sizeof(push_data.model));
    vkCmdPushConstants(cmd, m_render_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
        sizeof(push_data), &push_data);

    // Bind vertex buffer and draw
    VkBuffer vertex_buffers[] = {m_triangle_vertex_buffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertex_buffers, offsets);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    vkEndCommandBuffer(cmd);

    // Submit
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore wait_semaphores[] = {m_image_available_semaphores[m_current_frame]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    VkSemaphore signal_semaphores[] = {m_render_finished_semaphores[image_index]};
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;

    if (vkQueueSubmit(m_graphics_queue, 1, &submit_info, m_in_flight_fences[m_current_frame]) != VK_SUCCESS) {
        std::cerr << "Failed to submit draw command buffer\n";
        return;
    }

    // Present
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_semaphores;

    VkSwapchainKHR swapchains[] = {m_swapchain};
    present_info.swapchainCount = 1;
    present_info.pSwapchains = swapchains;
    present_info.pImageIndices = &image_index;

    result = vkQueuePresentKHR(m_present_queue, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebuffer_resized) {
        m_framebuffer_resized = false;
        recreate_swapchain();
    } else if (result != VK_SUCCESS) {
        std::cerr << "Failed to present swapchain image\n";
    }

    m_current_frame = (m_current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void Renderer::render(const Camera& camera, const ClusterManager& clusters) {
    // For now, if we have geometry data, render it; otherwise render test triangle
    if (!m_has_asset) {
        render_triangle(camera);
        return;
    }

    // Wait for previous frame
    // The swapchain can be gone after a failed recreation (e.g. minimized
    // window); retry creating it before touching any frame resources
    if (m_swapchain == VK_NULL_HANDLE) {
        recreate_swapchain();
        if (m_swapchain == VK_NULL_HANDLE) {
            return;
        }
    }

    // Bounded wait: if a previous submit failed the fence never signals,
    // and an infinite wait would hang the application forever. On timeout
    // skip the frame — the command buffer may still be pending.
    if (vkWaitForFences(m_device, 1, &m_in_flight_fences[m_current_frame], VK_TRUE,
                        2'000'000'000ull) != VK_SUCCESS) {
        std::cerr << "Timed out waiting for frame fence; skipping frame\n";
        return;
    }

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
        m_image_available_semaphores[m_current_frame], VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        // Without this check a device/surface loss would leave image_index
        // uninitialized and index m_framebuffers with garbage
        std::cerr << "Failed to acquire swapchain image\n";
        return;
    }

    vkResetFences(m_device, 1, &m_in_flight_fences[m_current_frame]);

    VkCommandBuffer cmd = m_command_buffers[m_current_frame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkRenderPassBeginInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = m_render_pass;
    render_pass_info.framebuffer = m_framebuffers[image_index];
    render_pass_info.renderArea.offset = {0, 0};
    render_pass_info.renderArea.extent = m_swapchain_extent;

    std::array<VkClearValue, 2> clear_values{};
    clear_values[0].color = {{0.1f, 0.1f, 0.15f, 1.0f}};
    clear_values[1].depthStencil = {1.0f, 0};
    render_pass_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
    render_pass_info.pClearValues = clear_values.data();

    vkCmdBeginRenderPass(cmd, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_render_pipeline);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_swapchain_extent.width);
    viewport.height = static_cast<float>(m_swapchain_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_swapchain_extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Push MVP and Model matrices (identity model)
    struct {
        float mvp[16];
        float model[16];
    } push_data;
    std::memcpy(push_data.mvp, camera.view_projection, sizeof(push_data.mvp));
    float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(push_data.model, identity, sizeof(push_data.model));
    vkCmdPushConstants(cmd, m_render_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
        sizeof(push_data), &push_data);

    // Draw visible clusters
    const auto& visible = clusters.visible_clusters();

    if (!visible.empty() && m_vertex_buffer != VK_NULL_HANDLE) {
        VkBuffer vertex_buffers[] = {m_vertex_buffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vertex_buffers, offsets);

        if (m_index_buffer != VK_NULL_HANDLE) {
            vkCmdBindIndexBuffer(cmd, m_index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, m_index_count, 1, 0, 0, 0);
        } else {
            vkCmdDraw(cmd, m_vertex_count, 1, 0, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore wait_semaphores[] = {m_image_available_semaphores[m_current_frame]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    VkSemaphore signal_semaphores[] = {m_render_finished_semaphores[image_index]};
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;

    if (vkQueueSubmit(m_graphics_queue, 1, &submit_info, m_in_flight_fences[m_current_frame]) != VK_SUCCESS) {
        std::cerr << "Failed to submit draw command buffer\n";
        return;
    }

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_semaphores;

    VkSwapchainKHR swapchains[] = {m_swapchain};
    present_info.swapchainCount = 1;
    present_info.pSwapchains = swapchains;
    present_info.pImageIndices = &image_index;

    result = vkQueuePresentKHR(m_present_queue, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebuffer_resized) {
        m_framebuffer_resized = false;
        recreate_swapchain();
    }

    m_current_frame = (m_current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void Renderer::render(const Camera& camera, const ClusterManager& clusters, const float* model_matrix) {
    // Same as render() but with a model transform applied

    if (!m_has_asset) {
        render_triangle(camera);
        return;
    }

    // Push constants: MVP matrix + Model matrix
    struct PushConstants {
        float mvp[16];
        float model[16];
    } push_data;

    // Identity matrix for when no model transform
    static const float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    const float* model = model_matrix ? model_matrix : identity;

    // Compute MVP = Projection * View * Model
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
        std::memcpy(push_data.mvp, camera.view_projection, 16 * sizeof(float));
    }

    // Copy model matrix for normal transformation
    std::memcpy(push_data.model, model, 16 * sizeof(float));

    // Wait for previous frame
    // The swapchain can be gone after a failed recreation (e.g. minimized
    // window); retry creating it before touching any frame resources
    if (m_swapchain == VK_NULL_HANDLE) {
        recreate_swapchain();
        if (m_swapchain == VK_NULL_HANDLE) {
            return;
        }
    }

    // Bounded wait: if a previous submit failed the fence never signals,
    // and an infinite wait would hang the application forever. On timeout
    // skip the frame — the command buffer may still be pending.
    if (vkWaitForFences(m_device, 1, &m_in_flight_fences[m_current_frame], VK_TRUE,
                        2'000'000'000ull) != VK_SUCCESS) {
        std::cerr << "Timed out waiting for frame fence; skipping frame\n";
        return;
    }

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
        m_image_available_semaphores[m_current_frame], VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        // Without this check a device/surface loss would leave image_index
        // uninitialized and index m_framebuffers with garbage
        std::cerr << "Failed to acquire swapchain image\n";
        return;
    }

    vkResetFences(m_device, 1, &m_in_flight_fences[m_current_frame]);

    VkCommandBuffer cmd = m_command_buffers[m_current_frame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkRenderPassBeginInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = m_render_pass;
    render_pass_info.framebuffer = m_framebuffers[image_index];
    render_pass_info.renderArea.offset = {0, 0};
    render_pass_info.renderArea.extent = m_swapchain_extent;

    std::array<VkClearValue, 2> clear_values{};
    clear_values[0].color = {{0.1f, 0.1f, 0.15f, 1.0f}};
    clear_values[1].depthStencil = {1.0f, 0};
    render_pass_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
    render_pass_info.pClearValues = clear_values.data();

    vkCmdBeginRenderPass(cmd, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_render_pipeline);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_swapchain_extent.width);
    viewport.height = static_cast<float>(m_swapchain_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_swapchain_extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Push MVP and Model matrices for rendering
    vkCmdPushConstants(cmd, m_render_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
        sizeof(push_data), &push_data);

    // Draw visible clusters
    const auto& visible = clusters.visible_clusters();

    if (!visible.empty() && m_vertex_buffer != VK_NULL_HANDLE) {
        VkBuffer vertex_buffers[] = {m_vertex_buffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vertex_buffers, offsets);

        if (m_index_buffer != VK_NULL_HANDLE) {
            vkCmdBindIndexBuffer(cmd, m_index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, m_index_count, 1, 0, 0, 0);
        } else {
            vkCmdDraw(cmd, m_vertex_count, 1, 0, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore wait_semaphores[] = {m_image_available_semaphores[m_current_frame]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    VkSemaphore signal_semaphores[] = {m_render_finished_semaphores[image_index]};
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;

    if (vkQueueSubmit(m_graphics_queue, 1, &submit_info, m_in_flight_fences[m_current_frame]) != VK_SUCCESS) {
        std::cerr << "Failed to submit draw command buffer\n";
        return;
    }

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_semaphores;

    VkSwapchainKHR swapchains[] = {m_swapchain};
    present_info.swapchainCount = 1;
    present_info.pSwapchains = swapchains;
    present_info.pImageIndices = &image_index;

    result = vkQueuePresentKHR(m_present_queue, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebuffer_resized) {
        m_framebuffer_resized = false;
        recreate_swapchain();
    }

    m_current_frame = (m_current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void Renderer::upload_asset(const VGeoAsset& asset) {
    // Create vertex buffer with interleaved pos + normal
    size_t vertex_count = asset.positions.size() / 3;

    // An empty asset would request a zero-size buffer (invalid in Vulkan)
    if (vertex_count == 0) {
        std::cerr << "upload_asset: asset has no vertices\n";
        return;
    }

    // Re-uploading: wait for in-flight frames that may still reference the
    // old buffers, then release them (they leaked before)
    if (m_vertex_buffer != VK_NULL_HANDLE || m_index_buffer != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        if (m_vertex_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_vertex_buffer, nullptr);
            vkFreeMemory(m_device, m_vertex_memory, nullptr);
            m_vertex_buffer = VK_NULL_HANDLE;
            m_vertex_memory = VK_NULL_HANDLE;
        }
        if (m_index_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_index_buffer, nullptr);
            vkFreeMemory(m_device, m_index_memory, nullptr);
            m_index_buffer = VK_NULL_HANDLE;
            m_index_memory = VK_NULL_HANDLE;
        }
        m_vertex_count = 0;
        m_index_count = 0;
        m_has_asset = false;
    }

    std::vector<float> interleaved(vertex_count * 6);

    for (size_t i = 0; i < vertex_count; i++) {
        // Position
        interleaved[i * 6 + 0] = asset.positions[i * 3 + 0];
        interleaved[i * 6 + 1] = asset.positions[i * 3 + 1];
        interleaved[i * 6 + 2] = asset.positions[i * 3 + 2];

        // Normal (decode from octahedral)
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
            interleaved[i * 6 + 3] = nx / len;
            interleaved[i * 6 + 4] = ny / len;
            interleaved[i * 6 + 5] = nz / len;
        } else {
            interleaved[i * 6 + 3] = 0.0f;
            interleaved[i * 6 + 4] = 1.0f;
            interleaved[i * 6 + 5] = 0.0f;
        }
    }

    VkDeviceSize buffer_size = interleaved.size() * sizeof(float);

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &buffer_info, nullptr, &m_vertex_buffer) != VK_SUCCESS) {
        std::cerr << "upload_asset: failed to create vertex buffer\n";
        return;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(m_device, m_vertex_buffer, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &alloc_info, nullptr, &m_vertex_memory) != VK_SUCCESS) {
        std::cerr << "upload_asset: failed to allocate vertex memory\n";
        vkDestroyBuffer(m_device, m_vertex_buffer, nullptr);
        m_vertex_buffer = VK_NULL_HANDLE;
        return;
    }
    vkBindBufferMemory(m_device, m_vertex_buffer, m_vertex_memory, 0);

    void* data = nullptr;
    if (vkMapMemory(m_device, m_vertex_memory, 0, buffer_size, 0, &data) != VK_SUCCESS) {
        std::cerr << "upload_asset: failed to map vertex memory\n";
        return;
    }
    memcpy(data, interleaved.data(), buffer_size);
    vkUnmapMemory(m_device, m_vertex_memory);

    m_vertex_count = static_cast<uint32_t>(vertex_count);

    // Create index buffer
    if (!asset.indices.empty()) {
        buffer_size = asset.indices.size() * sizeof(uint32_t);

        buffer_info.size = buffer_size;
        buffer_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

        if (vkCreateBuffer(m_device, &buffer_info, nullptr, &m_index_buffer) != VK_SUCCESS) {
            std::cerr << "upload_asset: failed to create index buffer\n";
            return;
        }

        vkGetBufferMemoryRequirements(m_device, m_index_buffer, &mem_reqs);

        alloc_info.allocationSize = mem_reqs.size;
        alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(m_device, &alloc_info, nullptr, &m_index_memory) != VK_SUCCESS) {
            std::cerr << "upload_asset: failed to allocate index memory\n";
            vkDestroyBuffer(m_device, m_index_buffer, nullptr);
            m_index_buffer = VK_NULL_HANDLE;
            return;
        }
        vkBindBufferMemory(m_device, m_index_buffer, m_index_memory, 0);

        if (vkMapMemory(m_device, m_index_memory, 0, buffer_size, 0, &data) != VK_SUCCESS) {
            std::cerr << "upload_asset: failed to map index memory\n";
            return;
        }
        memcpy(data, asset.indices.data(), buffer_size);
        vkUnmapMemory(m_device, m_index_memory);

        m_index_count = static_cast<uint32_t>(asset.indices.size());
    }

    m_has_asset = true;
    std::cout << "Uploaded " << m_vertex_count << " vertices, " << m_index_count << " indices\n";
}

void Renderer::set_show_clusters(bool show) {
    m_show_clusters = show;
}

void Renderer::set_show_bounds(bool show) {
    m_show_bounds = show;
}

void Renderer::set_wireframe(bool wireframe) {
    m_wireframe = wireframe;
    // TODO: Recreate pipeline with different polygon mode
}

} // namespace vgeo
