// vgeo_build CLI
// Convert OBJ/glTF meshes to .vgeo format

#include "mesh_import.h"
#include "meshlet_gen.h"
#include "hierarchy.h"
#include "vgeo_writer.h"

#include <iostream>
#include <string>

void print_usage() {
    std::cout << "Usage: vgeo_build <input.obj|gltf> <output.vgeo> [options]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  --compress       Enable LZ4 compression\n";
    std::cout << "  --quantize       Quantize vertex positions\n";
    std::cout << "  --max-verts N    Max vertices per meshlet (default: 64)\n";
    std::cout << "  --max-tris N     Max triangles per meshlet (default: 126)\n";
    std::cout << "  --verbose        Print detailed stats\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage();
        return 1;
    }

    std::string input = argv[1];
    std::string output = argv[2];

    std::cout << "vgeo_build v0.1\n";
    std::cout << "Input:  " << input << "\n";
    std::cout << "Output: " << output << "\n";

    // TODO: Implement conversion
    // 1. Load mesh
    // 2. Generate meshlets
    // 3. Build hierarchy
    // 4. Write .vgeo

    std::cout << "Build tool not yet implemented.\n";
    return 0;
}
