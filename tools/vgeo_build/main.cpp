// vgeo_build CLI
// Convert OBJ/glTF meshes to .vgeo format

#include "mesh_import.h"
#include "meshlet_gen.h"
#include "hierarchy.h"
#include "vgeo_writer.h"

#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>

using namespace vgeo;

void print_usage() {
    std::cout << "Usage: vgeo_build <input.obj|gltf> <output.vgeo> [options]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  --compress       Enable LZ4 compression\n";
    std::cout << "  --quantize       Quantize vertex positions\n";
    std::cout << "  --max-verts N    Max vertices per meshlet (default: 64)\n";
    std::cout << "  --max-tris N     Max triangles per meshlet (default: 126)\n";
    std::cout << "  --cluster-size N Meshlets per cluster (default: 8)\n";
    std::cout << "  --verbose        Print detailed stats\n";
}

// Parse command line arguments
struct Options {
    std::string input;
    std::string output;
    bool compress = false;
    bool quantize = false;
    uint32_t max_vertices = 64;
    uint32_t max_triangles = 126;
    uint32_t cluster_size = 8;
    bool verbose = false;
};

bool parse_args(int argc, char* argv[], Options& opts) {
    if (argc < 3) {
        return false;
    }

    opts.input = argv[1];
    opts.output = argv[2];

    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--compress") {
            opts.compress = true;
        } else if (arg == "--quantize") {
            opts.quantize = true;
        } else if (arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "--max-verts" && i + 1 < argc) {
            opts.max_vertices = std::stoi(argv[++i]);
        } else if (arg == "--max-tris" && i + 1 < argc) {
            opts.max_triangles = std::stoi(argv[++i]);
        } else if (arg == "--cluster-size" && i + 1 < argc) {
            opts.cluster_size = std::stoi(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }

    // Validate limits
    if (opts.max_vertices < 3 || opts.max_vertices > 255) {
        std::cerr << "Error: max-verts must be between 3 and 255\n";
        return false;
    }
    if (opts.max_triangles < 1 || opts.max_triangles > 512) {
        std::cerr << "Error: max-tris must be between 1 and 512\n";
        return false;
    }
    if (opts.cluster_size < 1 || opts.cluster_size > 64) {
        std::cerr << "Error: cluster-size must be between 1 and 64\n";
        return false;
    }

    return true;
}

// Timer helper
class Timer {
public:
    void start() { start_time = std::chrono::high_resolution_clock::now(); }
    double elapsed_ms() {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start_time).count();
    }
private:
    std::chrono::high_resolution_clock::time_point start_time;
};

int main(int argc, char* argv[]) {
    std::cout << "vgeo_build v0.1\n\n";

    Options opts;
    if (!parse_args(argc, argv, opts)) {
        print_usage();
        return 1;
    }

    std::cout << "Input:  " << opts.input << "\n";
    std::cout << "Output: " << opts.output << "\n\n";

    Timer timer;
    double load_time, meshlet_time, hierarchy_time, write_time;

    // Step 1: Load mesh
    std::cout << "Loading mesh...\n";
    timer.start();

    RawMesh mesh;
    if (!load_mesh(opts.input, mesh)) {
        std::cerr << "Error: Failed to load mesh from " << opts.input << "\n";
        return 1;
    }

    load_time = timer.elapsed_ms();

    std::cout << "  Vertices:  " << mesh.vertex_count() << "\n";
    std::cout << "  Triangles: " << mesh.triangle_count() << "\n";
    std::cout << "  Normals:   " << (mesh.normals.empty() ? "no" : "yes") << "\n";
    std::cout << "  UVs:       " << (mesh.uvs.empty() ? "no" : "yes") << "\n";

    if (mesh.vertex_count() == 0) {
        std::cerr << "Error: Mesh has no vertices\n";
        return 1;
    }

    // Step 2: Generate meshlets
    std::cout << "\nGenerating meshlets...\n";
    timer.start();

    MeshletParams meshlet_params;
    meshlet_params.max_vertices = opts.max_vertices;
    meshlet_params.max_triangles = opts.max_triangles;

    MeshletData meshlets;
    if (!generate_meshlets(mesh, meshlet_params, meshlets)) {
        std::cerr << "Error: Failed to generate meshlets\n";
        return 1;
    }

    meshlet_time = timer.elapsed_ms();

    std::cout << "  Meshlets:  " << meshlets.meshlets.size() << "\n";

    if (opts.verbose && !meshlets.meshlets.empty()) {
        // Calculate meshlet statistics
        uint32_t min_verts = UINT32_MAX, max_verts = 0;
        uint32_t min_tris = UINT32_MAX, max_tris = 0;
        uint32_t total_verts = 0, total_tris = 0;

        for (const auto& m : meshlets.meshlets) {
            min_verts = std::min(min_verts, m.vertex_count);
            max_verts = std::max(max_verts, m.vertex_count);
            min_tris = std::min(min_tris, m.triangle_count);
            max_tris = std::max(max_tris, m.triangle_count);
            total_verts += m.vertex_count;
            total_tris += m.triangle_count;
        }

        float avg_verts = static_cast<float>(total_verts) / meshlets.meshlets.size();
        float avg_tris = static_cast<float>(total_tris) / meshlets.meshlets.size();

        std::cout << "  Verts/meshlet: min=" << min_verts << " max=" << max_verts
                  << " avg=" << std::fixed << std::setprecision(1) << avg_verts << "\n";
        std::cout << "  Tris/meshlet:  min=" << min_tris << " max=" << max_tris
                  << " avg=" << std::fixed << std::setprecision(1) << avg_tris << "\n";
    }

    // Step 3: Build hierarchy
    std::cout << "\nBuilding hierarchy...\n";
    timer.start();

    HierarchyParams hierarchy_params;
    hierarchy_params.meshlets_per_cluster = opts.cluster_size;

    HierarchyData hierarchy;
    if (!build_hierarchy(meshlets, mesh, hierarchy_params, hierarchy)) {
        std::cerr << "Error: Failed to build hierarchy\n";
        return 1;
    }

    hierarchy_time = timer.elapsed_ms();

    std::cout << "  Clusters:  " << hierarchy.clusters.size() << "\n";
    std::cout << "  LOD levels: " << hierarchy.lod_levels << "\n";

    // Step 4: Write .vgeo file
    std::cout << "\nWriting .vgeo file...\n";
    timer.start();

    WriteParams write_params;
    write_params.compress = opts.compress;
    write_params.quantize_positions = opts.quantize;

    if (!write_vgeo(opts.output, mesh, meshlets, hierarchy, write_params)) {
        std::cerr << "Error: Failed to write " << opts.output << "\n";
        return 1;
    }

    write_time = timer.elapsed_ms();

    // Print summary
    std::cout << "\n=== Summary ===\n";
    std::cout << "Vertices:     " << mesh.vertex_count() << "\n";
    std::cout << "Triangles:    " << mesh.triangle_count() << "\n";
    std::cout << "Meshlets:     " << meshlets.meshlets.size() << "\n";
    std::cout << "Clusters:     " << hierarchy.clusters.size() << "\n";

    if (opts.verbose) {
        std::cout << "\n=== Timing ===\n";
        std::cout << "Load:      " << std::fixed << std::setprecision(2) << load_time << " ms\n";
        std::cout << "Meshlets:  " << std::fixed << std::setprecision(2) << meshlet_time << " ms\n";
        std::cout << "Hierarchy: " << std::fixed << std::setprecision(2) << hierarchy_time << " ms\n";
        std::cout << "Write:     " << std::fixed << std::setprecision(2) << write_time << " ms\n";
        std::cout << "Total:     " << std::fixed << std::setprecision(2)
                  << (load_time + meshlet_time + hierarchy_time + write_time) << " ms\n";
    }

    std::cout << "\n[OK] Successfully wrote " << opts.output << "\n";
    return 0;
}
