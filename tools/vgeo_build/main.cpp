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
#include <cstdlib>
#include <cerrno>
#include <limits>

using namespace vgeo;

void print_usage() {
    std::cout << "Usage: vgeo_build <input.obj|gltf> <output.vgeo> [options]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  --compress         Enable LZ4 compression (not yet implemented)\n";
    std::cout << "  --quantize         Quantize vertex positions (not yet implemented)\n";
    std::cout << "  --max-verts N      Max vertices per meshlet (default: 64)\n";
    std::cout << "  --max-tris N       Max triangles per meshlet (default: 126)\n";
    std::cout << "  --cluster-size N   Meshlets per cluster (default: 8)\n";
    std::cout << "  --branching N      Branching factor for LOD hierarchy (default: 4)\n";
    std::cout << "  --no-spatial       Disable spatial grouping (use sequential)\n";
    std::cout << "  --verbose          Print detailed stats\n";
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
    uint32_t branching_factor = 4;
    bool use_spatial_grouping = true;
    bool verbose = false;
};

// Parse a option value as a bounded unsigned integer without throwing
// (std::stoi aborts the process on non-numeric or overflowing input)
static bool parse_uint_arg(const char* name, const char* text, uint32_t& out) {
    errno = 0;
    char* end = nullptr;
    unsigned long v = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || text[0] == '-' ||
        v > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "Error: " << name << " expects a non-negative integer, got '"
                  << text << "'\n";
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool parse_args(int argc, char* argv[], Options& opts) {
    // Handle help before positional parsing so "vgeo_build --help" works
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            return false;
        }
    }

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
        } else if (arg == "--max-verts" || arg == "--max-tris" ||
                   arg == "--cluster-size" || arg == "--branching") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a value\n";
                return false;
            }
            uint32_t* target =
                (arg == "--max-verts")    ? &opts.max_vertices :
                (arg == "--max-tris")     ? &opts.max_triangles :
                (arg == "--cluster-size") ? &opts.cluster_size :
                                            &opts.branching_factor;
            if (!parse_uint_arg(arg.c_str(), argv[++i], *target)) {
                return false;
            }
        } else if (arg == "--no-spatial") {
            opts.use_spatial_grouping = false;
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
    if (opts.branching_factor < 2 || opts.branching_factor > 16) {
        std::cerr << "Error: branching must be between 2 and 16\n";
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

    if (opts.compress) {
        std::cout << "Warning: --compress is not implemented yet; writing uncompressed data\n";
    }
    if (opts.quantize) {
        std::cout << "Warning: --quantize is not implemented yet; writing float32 positions\n";
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
    hierarchy_params.branching_factor = opts.branching_factor;
    hierarchy_params.use_spatial_grouping = opts.use_spatial_grouping;

    HierarchyData hierarchy;
    if (!build_hierarchy(meshlets, mesh, hierarchy_params, hierarchy)) {
        std::cerr << "Error: Failed to build hierarchy\n";
        return 1;
    }

    hierarchy_time = timer.elapsed_ms();

    std::cout << "  Clusters:  " << hierarchy.clusters.size() << "\n";
    std::cout << "  LOD levels: " << hierarchy.lod_levels << "\n";
    std::cout << "  Leaf clusters: " << hierarchy.total_leaf_clusters << "\n";
    std::cout << "  Internal clusters: " << hierarchy.total_internal_clusters << "\n";
    std::cout << "  Root clusters: " << hierarchy.root_clusters.size() << "\n";

    if (opts.verbose) {
        // Print cluster count per LOD level
        std::vector<uint32_t> clusters_per_level(hierarchy.lod_levels, 0);
        for (const auto& c : hierarchy.clusters) {
            if (c.lod_level < hierarchy.lod_levels) {
                clusters_per_level[c.lod_level]++;
            }
        }
        std::cout << "  Clusters per level:\n";
        for (uint32_t l = 0; l < hierarchy.lod_levels; l++) {
            std::cout << "    Level " << l << ": " << clusters_per_level[l] << " clusters\n";
        }
    }

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
