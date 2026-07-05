// VGEO Standalone Viewer
// Vulkan window with camera controls and debug UI

#include "window.h"
#include "camera.h"
#include "renderer.h"
#include "vgeo_loader.h"
#include "cluster_manager.h"
#include "scene.h"

#include <iostream>
#include <string>
#include <chrono>
#include <cmath>

// GLFW key codes
#define GLFW_KEY_ESCAPE 256
#define GLFW_KEY_C 67
#define GLFW_KEY_B 66
#define GLFW_KEY_W 87
#define GLFW_KEY_R 82
#define GLFW_KEY_LEFT 263
#define GLFW_KEY_RIGHT 262
#define GLFW_KEY_UP 265
#define GLFW_KEY_DOWN 264
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
    std::cout << "  Arrow Keys         - Rotate model\n";
    std::cout << "  R                  - Reset model rotation\n";
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

    // Scene graph for multi-object management
    vgeo::Scene scene;

    // Cluster manager (for per-asset visibility)
    vgeo::ClusterManager clusters;

    // Object tracking
    vgeo::ObjectId main_object = vgeo::INVALID_OBJECT_ID;

    // Model rotation (Euler angles in radians)
    float model_rotation_x = 0.0f;
    float model_rotation_y = 0.0f;

    // Load .vgeo file if provided
    if (!path.empty()) {
        std::cout << "Loading: " << path << "\n";
        main_object = scene.add_object_from_file(path, nullptr, "main");

        if (main_object != vgeo::INVALID_OBJECT_ID) {
            // Get the loaded asset for renderer
            vgeo::SceneObject* obj = scene.get_object(main_object);
            const vgeo::VGeoAsset* asset = scene.get_asset(obj->asset_id);

            if (asset) {
                renderer.upload_asset(*asset);
                clusters.set_asset(asset);

                // Fit camera to object bounds
                float center_x = (obj->bounds_min[0] + obj->bounds_max[0]) * 0.5f;
                float center_y = (obj->bounds_min[1] + obj->bounds_max[1]) * 0.5f;
                float center_z = (obj->bounds_min[2] + obj->bounds_max[2]) * 0.5f;

                float extent_x = obj->bounds_max[0] - obj->bounds_min[0];
                float extent_y = obj->bounds_max[1] - obj->bounds_min[1];
                float extent_z = obj->bounds_max[2] - obj->bounds_min[2];
                float max_extent = std::max({extent_x, extent_y, extent_z});

                camera.target[0] = center_x;
                camera.target[1] = center_y;
                camera.target[2] = center_z;
                camera.position[0] = center_x;
                camera.position[1] = center_y;
                camera.position[2] = center_z + max_extent * 2.0f;
                camera.update();

                std::cout << "Object bounds: [" << obj->bounds_min[0] << ", " << obj->bounds_min[1] << ", " << obj->bounds_min[2]
                          << "] - [" << obj->bounds_max[0] << ", " << obj->bounds_max[1] << ", " << obj->bounds_max[2] << "]\n";
            }
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

        const float rotation_speed = 0.1f;  // radians per key press

        switch (key) {
            case GLFW_KEY_ESCAPE:
                window.request_close();
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
            case GLFW_KEY_LEFT:
                model_rotation_y -= rotation_speed;
                break;
            case GLFW_KEY_RIGHT:
                model_rotation_y += rotation_speed;
                break;
            case GLFW_KEY_UP:
                model_rotation_x -= rotation_speed;
                break;
            case GLFW_KEY_DOWN:
                model_rotation_x += rotation_speed;
                break;
            case GLFW_KEY_R:
                model_rotation_x = 0.0f;
                model_rotation_y = 0.0f;
                std::cout << "Model rotation reset\n";
                break;
        }
    });

    // Frame timing
    auto last_time = std::chrono::high_resolution_clock::now();
    int frame_count = 0;
    float fps_timer = 0.0f;

    // Resize tracking
    uint32_t last_width = window.width();
    uint32_t last_height = window.height();

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
            if (main_object != vgeo::INVALID_OBJECT_ID) {
                std::cout << " | Visible meshlets: " << clusters.total_meshlets_visible()
                          << " | Culled: " << clusters.clusters_culled();
            }
            std::cout << "\r" << std::flush;
            frame_count = 0;
            fps_timer = 0.0f;
        }

        // Poll input
        window.poll_events();

        // Skip frames while minimized: a 0x0 framebuffer would produce a
        // division by zero below and an invalid zero-extent swapchain
        if (window.width() == 0 || window.height() == 0) {
            continue;
        }

        // Handle resize. Compare integers — reconstructing the width from
        // the float aspect ratio misdetects a resize every frame for many
        // window sizes, causing endless swapchain recreation.
        if (window.width() != last_width || window.height() != last_height) {
            last_width = window.width();
            last_height = window.height();
            camera.aspect = static_cast<float>(window.width()) / static_cast<float>(window.height());
            camera.update();
            renderer.resize(window.width(), window.height());
        }

        // Update cluster visibility (CPU culling for now)
        if (main_object != vgeo::INVALID_OBJECT_ID) {
            clusters.update(camera, 1.0f);  // 1 pixel error threshold
        }

        // Compute model matrix from rotation
        float model_matrix[16];
        {
            // Simple Y-axis then X-axis rotation
            float cy = std::cos(model_rotation_y);
            float sy = std::sin(model_rotation_y);
            float cx = std::cos(model_rotation_x);
            float sx = std::sin(model_rotation_x);

            // Combined rotation: Ry * Rx (Y first, then X)
            model_matrix[0] = cy;
            model_matrix[1] = sx * sy;
            model_matrix[2] = -cx * sy;
            model_matrix[3] = 0.0f;

            model_matrix[4] = 0.0f;
            model_matrix[5] = cx;
            model_matrix[6] = sx;
            model_matrix[7] = 0.0f;

            model_matrix[8] = sy;
            model_matrix[9] = -sx * cy;
            model_matrix[10] = cx * cy;
            model_matrix[11] = 0.0f;

            model_matrix[12] = 0.0f;
            model_matrix[13] = 0.0f;
            model_matrix[14] = 0.0f;
            model_matrix[15] = 1.0f;
        }

        // Render with model transform
        renderer.render(camera, clusters, model_matrix);
    }

    std::cout << "\nShutting down...\n";

    // Cleanup
    renderer.destroy();
    window.destroy();

    std::cout << "Goodbye!\n";
    return 0;
}
