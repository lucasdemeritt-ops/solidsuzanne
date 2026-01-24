// vgeo_validate CLI
// Validate .vgeo file structure and data

#include "vgeo_format.h"

#include <iostream>
#include <fstream>
#include <string>

void print_usage() {
    std::cout << "Usage: vgeo_validate <file.vgeo> [options]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  --stats    Print detailed statistics\n";
    std::cout << "  --dump     Dump chunk information\n";
}

bool validate_file(const std::string& path, bool verbose) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Cannot open file: " << path << "\n";
        return false;
    }

    vgeo::FileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (!file) {
        std::cerr << "Error: Cannot read header\n";
        return false;
    }

    if (!vgeo::is_valid_header(header)) {
        std::cerr << "Error: Invalid header (bad magic or version)\n";
        return false;
    }

    if (verbose) {
        std::cout << "Version:  " << header.version_major << "." << header.version_minor << "\n";
        std::cout << "Meshlets: " << header.meshlet_count << "\n";
        std::cout << "Clusters: " << header.cluster_count << "\n";
        std::cout << "Vertices: " << header.vertex_count << "\n";
        std::cout << "Indices:  " << header.index_count << "\n";
        std::cout << "Chunks:   " << header.chunk_count << "\n";
    }

    // TODO: Validate chunks, bounds, hierarchy integrity

    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string path = argv[1];
    bool verbose = argc > 2 && std::string(argv[2]) == "--stats";

    std::cout << "vgeo_validate v0.1\n";
    std::cout << "Validating: " << path << "\n\n";

    if (validate_file(path, verbose)) {
        std::cout << "\n✓ File is valid\n";
        return 0;
    } else {
        std::cout << "\n✗ Validation failed\n";
        return 1;
    }
}
