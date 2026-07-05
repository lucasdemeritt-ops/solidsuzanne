// vgeo_validate CLI
// Validate .vgeo file structure and data

#include "vgeo_format.h"
#include "vgeo_loader.h"

#include <iostream>
#include <fstream>
#include <string>
#include <iomanip>

using namespace vgeo;

void print_usage() {
    std::cout << "Usage: vgeo_validate <file.vgeo> [options]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  --stats    Print detailed statistics\n";
    std::cout << "  --dump     Dump chunk information\n";
}

// Convert chunk type to string
const char* chunk_type_str(uint32_t type) {
    switch (type) {
        case ChunkType::VERT: return "VERT";
        case ChunkType::NORM: return "NORM";
        case ChunkType::UVCO: return "UVCO";
        case ChunkType::INDX: return "INDX";
        case ChunkType::MSLT: return "MSLT";
        case ChunkType::CLST: return "CLST";
        case ChunkType::BVOL: return "BVOL";
        case ChunkType::CBND: return "CBND";
        case ChunkType::CONE: return "CONE";
        default: return "????";
    }
}

// Print flags as readable string
void print_flags(uint32_t flags) {
    std::cout << "Flags:    0x" << std::hex << flags << std::dec << " (";
    bool first = true;
    auto add_flag = [&](bool cond, const char* name) {
        if (cond) {
            if (!first) std::cout << " | ";
            std::cout << name;
            first = false;
        }
    };
    add_flag(flags & Flags::COMPRESSED, "COMPRESSED");
    add_flag(flags & Flags::QUANTIZED_POS, "QUANTIZED_POS");
    add_flag(flags & Flags::HAS_NORMALS, "HAS_NORMALS");
    add_flag(flags & Flags::HAS_UVS, "HAS_UVS");
    add_flag(flags & Flags::HAS_TANGENTS, "HAS_TANGENTS");
    add_flag(flags & Flags::INDEX_16BIT, "INDEX_16BIT");
    if (first) std::cout << "none";
    std::cout << ")\n";
}

bool validate_file(const std::string& path, bool show_stats, bool dump_chunks) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Cannot open file: " << path << "\n";
        return false;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::cout << "File size: " << file_size << " bytes\n\n";

    if (file_size < sizeof(FileHeader)) {
        std::cerr << "Error: File too small for header (need " << sizeof(FileHeader) << " bytes)\n";
        return false;
    }

    // Read header
    FileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (!file) {
        std::cerr << "Error: Cannot read header\n";
        return false;
    }

    // Validate magic
    if (header.magic != VGEO_MAGIC) {
        std::cerr << "Error: Invalid magic number (expected 0x" << std::hex << VGEO_MAGIC
                  << ", got 0x" << header.magic << std::dec << ")\n";
        return false;
    }

    std::cout << "=== Header ===\n";
    std::cout << "Magic:    VGEO (0x" << std::hex << header.magic << std::dec << ")\n";
    std::cout << "Version:  " << header.version_major << "." << header.version_minor << "\n";
    print_flags(header.flags);
    std::cout << "Meshlets: " << header.meshlet_count << "\n";
    std::cout << "Clusters: " << header.cluster_count << "\n";
    std::cout << "Vertices: " << header.vertex_count << "\n";
    std::cout << "Indices:  " << header.index_count << "\n";
    std::cout << "Chunks:   " << header.chunk_count << "\n";
    std::cout << "Bounds:   (" << header.bounds.min[0] << ", " << header.bounds.min[1] << ", " << header.bounds.min[2] << ") - ("
              << header.bounds.max[0] << ", " << header.bounds.max[1] << ", " << header.bounds.max[2] << ")\n";

    // Validate version
    if (header.version_major != VGEO_VERSION_MAJOR) {
        std::cerr << "\nError: Unsupported major version (expected " << VGEO_VERSION_MAJOR
                  << ", got " << header.version_major << ")\n";
        return false;
    }

    // Validate chunk directory fits in file
    size_t directory_size = header.chunk_count * sizeof(ChunkEntry);
    size_t directory_end = sizeof(FileHeader) + directory_size;
    if (file_size < directory_end) {
        std::cerr << "\nError: File too small for chunk directory\n";
        return false;
    }

    // Read chunk directory
    std::vector<ChunkEntry> chunks(header.chunk_count);
    file.read(reinterpret_cast<char*>(chunks.data()), directory_size);

    if (!file) {
        std::cerr << "\nError: Cannot read chunk directory\n";
        return false;
    }

    std::cout << "\n=== Chunks ===\n";

    bool valid = true;
    bool has_vert = false, has_indx = false, has_mslt = false;
    size_t total_chunk_size = 0;

    for (size_t i = 0; i < chunks.size(); i++) {
        const ChunkEntry& chunk = chunks[i];

        if (dump_chunks) {
            std::cout << "[" << i << "] " << chunk_type_str(chunk.type)
                      << "  offset=" << std::setw(8) << chunk.offset
                      << "  size=" << std::setw(8) << chunk.size;
            if (chunk.compressed_size > 0) {
                std::cout << "  compressed=" << chunk.compressed_size;
            }
            std::cout << "\n";
        }

        // Validate offset alignment
        if (chunk.offset % 16 != 0) {
            std::cerr << "Error: Chunk " << i << " (" << chunk_type_str(chunk.type)
                      << ") not 16-byte aligned (offset=" << chunk.offset << ")\n";
            valid = false;
        }

        // Validate offset and size
        if (chunk.offset >= file_size) {
            std::cerr << "Error: Chunk " << i << " (" << chunk_type_str(chunk.type)
                      << ") offset beyond file end\n";
            valid = false;
        } else if (static_cast<uint64_t>(chunk.offset) + chunk.size > file_size) {
            std::cerr << "Error: Chunk " << i << " (" << chunk_type_str(chunk.type)
                      << ") extends beyond file end\n";
            valid = false;
        }

        // Track required chunks
        if (chunk.type == ChunkType::VERT) has_vert = true;
        if (chunk.type == ChunkType::INDX) has_indx = true;
        if (chunk.type == ChunkType::MSLT) has_mslt = true;

        total_chunk_size += chunk.size;
    }

    if (!dump_chunks) {
        std::cout << header.chunk_count << " chunks, " << total_chunk_size << " bytes of data\n";
    }

    // Check required chunks
    if (!has_vert) {
        std::cerr << "Error: Missing required VERT chunk\n";
        valid = false;
    }
    if (!has_indx) {
        std::cerr << "Error: Missing required INDX chunk\n";
        valid = false;
    }
    if (!has_mslt) {
        std::cerr << "Error: Missing required MSLT chunk\n";
        valid = false;
    }

    // Print statistics if requested
    if (show_stats && valid) {
        std::cout << "\n=== Statistics ===\n";

        // Calculate compression ratio if applicable
        size_t header_overhead = sizeof(FileHeader) + directory_size;
        size_t data_region = file_size - header_overhead;

        std::cout << "Header + directory: " << header_overhead << " bytes\n";
        std::cout << "Data region:        " << data_region << " bytes\n";

        if (header.vertex_count > 0) {
            float bytes_per_vertex = static_cast<float>(file_size) / header.vertex_count;
            std::cout << "Bytes per vertex:   " << std::fixed << std::setprecision(2) << bytes_per_vertex << "\n";
        }

        if (header.meshlet_count > 0) {
            float avg_verts = static_cast<float>(header.vertex_count) / header.meshlet_count;
            float avg_tris = static_cast<float>(header.index_count) / (3 * header.meshlet_count);
            std::cout << "Avg verts/meshlet:  " << std::fixed << std::setprecision(1) << avg_verts << "\n";
            std::cout << "Avg tris/meshlet:   " << std::fixed << std::setprecision(1) << avg_tris << "\n";
        }

        // Try to load and validate hierarchy
        auto asset = load_vgeo(path);
        if (!asset) {
            std::cerr << "Error: Chunk directory looks valid but deep load failed\n";
            valid = false;
        }
        if (asset) {
            std::cout << "\n=== Hierarchy ===\n";
            std::cout << "Clusters loaded:    " << asset->clusters.size() << "\n";

            if (!asset->clusters.empty()) {
                // Find root(s)
                int root_count = 0;
                for (const auto& cluster : asset->clusters) {
                    if (cluster.parent_id == INVALID_ID) {
                        root_count++;
                    }
                }
                std::cout << "Root clusters:      " << root_count << "\n";

                // Find max LOD level
                uint8_t max_lod = 0;
                for (const auto& cluster : asset->clusters) {
                    max_lod = std::max(max_lod, cluster.lod_level);
                }
                std::cout << "LOD levels:         " << (max_lod + 1) << "\n";
            }

            // Validate meshlet bounds
            if (asset->meshlet_bounds.size() != asset->meshlets.size()) {
                std::cerr << "Warning: Meshlet bounds count (" << asset->meshlet_bounds.size()
                          << ") != meshlet count (" << asset->meshlets.size() << ")\n";
            }

            // Validate normal cones
            if (asset->meshlet_cones.size() != asset->meshlets.size()) {
                std::cerr << "Warning: Normal cones count (" << asset->meshlet_cones.size()
                          << ") != meshlet count (" << asset->meshlets.size() << ")\n";
            }
        }
    }

    return valid;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string path;
    bool show_stats = false;
    bool dump_chunks = false;

    // Accept options and the path in any order; reject unknown options
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--stats") {
            show_stats = true;
        } else if (arg == "--dump") {
            dump_chunks = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage();
            return 1;
        } else if (path.empty()) {
            path = arg;
        } else {
            std::cerr << "Multiple input files given\n";
            print_usage();
            return 1;
        }
    }

    if (path.empty()) {
        print_usage();
        return 1;
    }

    std::cout << "vgeo_validate v0.1\n";
    std::cout << "Validating: " << path << "\n\n";

    if (validate_file(path, show_stats, dump_chunks)) {
        std::cout << "\n[OK] File is valid\n";
        return 0;
    } else {
        std::cout << "\n[FAIL] Validation failed\n";
        return 1;
    }
}
