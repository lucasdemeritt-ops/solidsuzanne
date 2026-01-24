// VGEO Window
// Platform window creation and input handling using GLFW

#include "window.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace vgeo {

bool Window::create(const WindowConfig& config) {
    // Initialize GLFW
    if (!glfwInit()) {
        return false;
    }

    // Check Vulkan support
    if (!glfwVulkanSupported()) {
        glfwTerminate();
        return false;
    }

    // Configure for Vulkan (no OpenGL context)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);

    // Create window
    GLFWwindow* window = glfwCreateWindow(
        static_cast<int>(config.width),
        static_cast<int>(config.height),
        config.title,
        nullptr,
        nullptr
    );

    if (!window) {
        glfwTerminate();
        return false;
    }

    m_handle = window;
    m_width = config.width;
    m_height = config.height;
    m_should_close = false;

    // Store this pointer for callbacks
    glfwSetWindowUserPointer(window, this);

    // Set up GLFW callbacks
    glfwSetCursorPosCallback(window, [](GLFWwindow* win, double x, double y) {
        Window* w = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (w && w->m_mouse_callback) {
            w->m_mouse_callback(static_cast<float>(x), static_cast<float>(y));
        }
    });

    glfwSetScrollCallback(window, [](GLFWwindow* win, double /*xoffset*/, double yoffset) {
        Window* w = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (w && w->m_scroll_callback) {
            w->m_scroll_callback(static_cast<float>(yoffset));
        }
    });

    glfwSetKeyCallback(window, [](GLFWwindow* win, int key, int /*scancode*/, int action, int /*mods*/) {
        Window* w = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (w && w->m_key_callback) {
            bool pressed = (action == GLFW_PRESS || action == GLFW_REPEAT);
            w->m_key_callback(key, pressed);
        }
    });

    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* win, int width, int height) {
        Window* w = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (w) {
            w->m_width = static_cast<uint32_t>(width);
            w->m_height = static_cast<uint32_t>(height);
        }
    });

    glfwSetWindowCloseCallback(window, [](GLFWwindow* win) {
        Window* w = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (w) {
            w->m_should_close = true;
        }
    });

    return true;
}

void Window::destroy() {
    if (m_handle) {
        glfwDestroyWindow(static_cast<GLFWwindow*>(m_handle));
        m_handle = nullptr;
    }
    glfwTerminate();
}

void* Window::native_handle() const {
    return m_handle;
}

bool Window::should_close() const {
    if (!m_handle) return true;
    return m_should_close || glfwWindowShouldClose(static_cast<GLFWwindow*>(m_handle));
}

void Window::poll_events() {
    glfwPollEvents();
}

uint32_t Window::width() const {
    return m_width;
}

uint32_t Window::height() const {
    return m_height;
}

void Window::set_mouse_callback(MouseCallback cb) {
    m_mouse_callback = std::move(cb);
}

void Window::set_scroll_callback(ScrollCallback cb) {
    m_scroll_callback = std::move(cb);
}

void Window::set_key_callback(KeyCallback cb) {
    m_key_callback = std::move(cb);
}

bool Window::is_mouse_button_pressed(int button) const {
    if (!m_handle) return false;
    return glfwGetMouseButton(static_cast<GLFWwindow*>(m_handle), button) == GLFW_PRESS;
}

void Window::get_mouse_pos(float& x, float& y) const {
    if (!m_handle) {
        x = y = 0.0f;
        return;
    }
    double dx, dy;
    glfwGetCursorPos(static_cast<GLFWwindow*>(m_handle), &dx, &dy);
    x = static_cast<float>(dx);
    y = static_cast<float>(dy);
}

} // namespace vgeo
