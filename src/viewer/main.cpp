// VGEO Standalone Viewer
// Vulkan window with camera controls and debug UI

#include "window.h"
#include "camera.h"
#include "renderer.h"
#include "vgeo_loader.h"
#include "cluster_manager.h"

#include <iostream>
#include <string>
#include <chrono>

// GLFW key codes
#define GLFW_KEY_ESCAPE 256
#define GLFW_KEY_C 67
#define GLFW_KEY_B 66
#define GLFW_KEY_W 87
#define GLFW_MOUSE_BUTTON_LEFT 0
#define GLFW_MOUSE_BUTTON_RIGHT 1

void print_usage() {
    std::cout << "Usage: vgeo_viewer <scene.vscene>\n";
    std::cout << "       vgeo_viewer <mesh.vgeo>\n";
    std::cout << "       vgeo_viewer (no args for test triangle)\n";
    std::cout << "\nControls:\n";
    std::cout << "  Left Mouse + Drag  - Orbit camera\n";
    std::cout << "  Right Mouse + Drag - Pan camera\n";
    std::cout << "  Scroll             - Zoom\n";
    std::cout << "  C                  - Toggle cluster colors\n";
    std::cout << "  B                  - Toggle bounding boxes\n";
    std::cout << "  W                  - Toggle wireframe\n";
    std::cout << "  ESC                - Quit\n";
}

int main(int argc, char* argv[]) {
    std::string path;
    if (argc >= 2) {
        if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            print_usage();
            return 0;
        }
        path = argv[1];
    }

    std::cout << "VGEO Viewer v0.1\n";

    // Create window
    vgeo::Window window;
    vgeo::WindowConfig config;
    config.title = "VGEO Viewer";
    config.width = 1280;
    config.height = 720;

    if (!window.create(config)) {
        std::cerr << "Failed to create window\n";
        return 1;
    }

    std::cout << "Window created: " << config.width << "x" << config.height << "\n";

    // Initialize renderer
    vgeo::Renderer renderer;
    if (!renderer.init(window.native_handle(), window.width(), window.height())) {
        std::cerr << "Failed to initialize renderer\n";
        window.destroy();
        return 1;
    }

    // Set up camera
    vgeo::Camera camera;
    camera.position[0] = 0.0f;
    camera.position[1] = 0.0f;
    camera.position[2] = 5.0f;
    camera.target[0] = 0.0f;
    camera.target[1] = 0.0f;
    camera.target[2] = 0.0f;
    camera.aspect = static_cast<float>(window.width()) / static_cast<float>(window.height());
    camera.update();

    // Cluster manager
    vgeo::ClusterManager clusters;

    // Load .vgeo file if provided
    std::unique_ptr<vgeo::VGeoAsset> asset;
    if (!path.empty()) {
        std::cout << "Loading: " << path << "\n";
        asset = vgeo::load_vgeo(path);
        if (asset) {
            renderer.upload_asset(*asset);
            clusters.set_asset(asset.get());

            // Fit camera to bounds
            const auto& bounds = asset->header.bounds;
            float center_x = (bounds.min[0] + bounds.max[0]) * 0.5f;
            float center_y = (bounds.min[1] + bounds.max[1]) * 0.5f;
            float center_z = (bounds.min[2] + bounds.max[2]) * 0.5f;

            float extent_x = bounds.max[0] - bounds.min[0];
            float extent_y = bounds.max[1] - bounds.min[1];
            float extent_z = bounds.max[2] - bounds.min[2];
            float max_extent = std::max({extent_x, extent_y, extent_z});

            camera.target[0] = center_x;
            camera.target[1] = center_y;
            camera.target[2] = center_z;
            camera.position[0] = center_x;
            camera.position[1] = center_y;
            camera.position[2] = center_z + max_extent * 2.0f;
            camera.update();

            std::cout << "Asset bounds: [" << bounds.min[0] << ", " << bounds.min[1] << ", " << bounds.min[2]
                      << "] - [" << bounds.max[0] << ", " << bounds.max[1] << ", " << bounds.max[2] << "]\n";
        } else {
            std::cerr << "Failed to load: " << path << "\n";
        }
    } else {
        std::cout << "No file specified, showing test triangle\n";
    }

    // Input state
    float last_mouse_x = 0.0f, last_mouse_y = 0.0f;
    bool first_mouse = true;
    bool show_clusters = false;
    bool show_bounds = false;
    bool wireframe = false;

    // Set up input callbacks
    window.set_mouse_callback([&](float x, float y) {
        if (first_mouse) {
            last_mouse_x = x;
            last_mouse_y = y;
            first_mouse = false;
            return;
        }

        float dx = x - last_mouse_x;
        float dy = y - last_mouse_y;
        last_mouse_x = x;
        last_mouse_y = y;

        // Left mouse: orbit
        if (window.is_mouse_button_pressed(GLFW_MOUSE_BUTTON_LEFT)) {
            camera.orbit(-dx * 0.005f, -dy * 0.005f);
            camera.update();
        }

        // Right mouse: pan
        if (window.is_mouse_button_pressed(GLFW_MOUSE_BUTTON_RIGHT)) {
            camera.pan(dx, dy);
            camera.update();
        }
    });

    window.set_scroll_callback([&](float delta) {
        camera.zoom(delta);
        camera.update();
    });

    window.set_key_callback([&](int key, bool pressed) {
        if (!pressed) return;

        switch (key) {
            case GLFW_KEY_ESCAPE:
                // Request close - handled by window
                break;
            case GLFW_KEY_C:
                show_clusters = !show_clusters;
                renderer.set_show_clusters(show_clusters);
                std::cout << "Cluster colors: " << (show_clusters ? "ON" : "OFF") << "\n";
                break;
            case GLFW_KEY_B:
                show_bounds = !show_bounds;
                renderer.set_show_bounds(show_bounds);
                std::cout << "Bounding boxes: " << (show_bounds ? "ON" : "OFF") << "\n";
                break;
            case GLFW_KEY_W:
                wireframe = !wireframe;
                renderer.set_wireframe(wireframe);
                std::cout << "Wireframe: " << (wireframe ? "ON" : "OFF") << "\n";
                break;
        }
    });

    // Frame timing
    auto last_time = std::chrono::high_resolution_clock::now();
    int frame_count = 0;
    float fps_timer = 0.0f;

    // Main loop
    std::cout << "Entering main loop...\n";
    while (!window.should_close()) {
        // Calculate delta time
        auto current_time = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(current_time - last_time).count();
        last_time = current_time;

        // FPS counter
        frame_count++;
        fps_timer += dt;
        if (fps_timer >= 1.0f) {
            float fps = static_cast<float>(frame_count) / fps_timer;
            std::cout << "FPS: " << fps;
            if (asset) {
                std::cout << " | Visible meshlets: " << clusters.total_meshlets_visible()
                          << " | Culled: " << clusters.clusters_culled();
            }
            std::cout << "\r" << std::flush;
            frame_count = 0;
            fps_timer = 0.0f;
        }

        // Poll input
        window.poll_events();

        // Handle resize
        if (window.width() != camera.aspect * window.height()) {
            camera.aspect = static_cast<float>(window.width()) / static_cast<float>(window.height());
            camera.update();
            renderer.resize(window.width(), window.height());
        }

        // Update cluster visibility (CPU culling for now)
        if (asset) {
            clusters.update(camera, 1.0f);  // 1 pixel error threshold
        }

        // Render
        renderer.render(camera, clusters);
    }

    std::cout << "\nShutting down...\n";

    // Cleanup
    renderer.destroy();
    window.destroy();

    std::cout << "Goodbye!\n";
    return 0;
}
