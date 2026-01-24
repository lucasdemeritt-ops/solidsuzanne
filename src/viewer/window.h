#pragma once

#include <cstdint>
#include <functional>

namespace vgeo {

struct WindowConfig {
    const char* title = "VGEO Viewer";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool resizable = true;
};

class Window {
public:
    bool create(const WindowConfig& config);
    void destroy();

    void* native_handle() const;
    bool should_close() const;
    void poll_events();

    uint32_t width() const;
    uint32_t height() const;

    // Input callbacks
    using MouseCallback = std::function<void(float x, float y)>;
    using ScrollCallback = std::function<void(float delta)>;
    using KeyCallback = std::function<void(int key, bool pressed)>;

    void set_mouse_callback(MouseCallback cb);
    void set_scroll_callback(ScrollCallback cb);
    void set_key_callback(KeyCallback cb);

private:
    void* m_handle = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_should_close = false;
};

} // namespace vgeo
