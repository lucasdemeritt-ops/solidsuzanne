// VGEO Standalone Viewer
// Vulkan window with camera controls and debug UI

#include "renderer.h"
#include "vgeo_loader.h"
#include "cluster_manager.h"
#include "camera.h"
#include "window.h"

#include <iostream>
#include <string>

void print_usage() {
    std::cout << "Usage: vgeo_viewer <scene.vscene>\n";
    std::cout << "       vgeo_viewer <mesh.vgeo>\n";
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
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string path = argv[1];
    std::cout << "VGEO Viewer v0.1\n";
    std::cout << "Loading: " << path << "\n";

    // TODO: Implement viewer
    // 1. Create window
    // 2. Initialize Vulkan renderer
    // 3. Load .vgeo or .vscene
    // 4. Main loop: input -> cull -> render

    std::cout << "Viewer not yet implemented.\n";
    return 0;
}
