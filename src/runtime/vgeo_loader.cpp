// VGEO File Loader
// Load .vgeo files into GPU-ready structures

#include "vgeo_loader.h"

#include <fstream>
#include <cstring>
#include <unordered_map>

namespace vgeo {

// Helper to get chunk type name for error messages
static const char* chunk_type_name(uint32_t type) {
    switch (type) {
        case ChunkType::VERT: return "VERT";
        case ChunkType::NORM: return "NORM";
        case ChunkType::UVCO: return "UVCO";
        case ChunkType::INDX: return "INDX";
        case ChunkType::MSLT: return "MSLT";
        case ChunkType::CLST: return "CLST";
        case ChunkType::BVOL: return "BVOL";
        case ChunkType::CONE: return "CONE";
        default: return "UNKNOWN";
    }
}

std::unique_ptr<VGeoAsset> load_vgeo(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return nullptr;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    if (file_size < sizeof(FileHeader)) {
        return nullptr;
    }

    // Read header
    FileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (!is_valid_header(header)) {
        return nullptr;
    }

    // Read chunk directory
    if (file_size < sizeof(FileHeader) + header.chunk_count * sizeof(ChunkEntry)) {
        return nullptr;
    }

    std::vector<ChunkEntry> chunks(header.chunk_count);
    file.read(reinterpret_cast<char*>(chunks.data()), header.chunk_count * sizeof(ChunkEntry));

    // Build chunk map for easy lookup
    std::unordered_map<uint32_t, const ChunkEntry*> chunk_map;
    for (const auto& chunk : chunks) {
        chunk_map[chunk.type] = &chunk;
    }

    // Create asset
    auto asset = std::make_unique<VGeoAsset>();
    asset->header = header;

    // Helper to read a chunk
    auto read_chunk = [&](uint32_t type, void* data, size_t expected_size) -> bool {
        auto it = chunk_map.find(type);
        if (it == chunk_map.end()) {
            return false;
        }

        const ChunkEntry* entry = it->second;

        if (entry->offset + entry->size > file_size) {
            return false;
        }

        if (entry->size != expected_size) {
            return false;
        }

        file.seekg(entry->offset);
        file.read(reinterpret_cast<char*>(data), entry->size);

        return file.good();
    };

    // Helper to read a chunk into a vector
    auto read_chunk_vec = [&](uint32_t type, auto& vec, size_t element_size) -> bool {
        auto it = chunk_map.find(type);
        if (it == chunk_map.end()) {
            return false;
        }

        const ChunkEntry* entry = it->second;

        if (entry->offset + entry->size > file_size) {
            return false;
        }

        size_t count = entry->size / element_size;
        vec.resize(count);

        file.seekg(entry->offset);
        file.read(reinterpret_cast<char*>(vec.data()), entry->size);

        return file.good();
    };

    // Load VERT chunk - positions (float x 3 per vertex)
    {
        auto it = chunk_map.find(ChunkType::VERT);
        if (it == chunk_map.end()) {
            return nullptr;  // VERT is required
        }

        const ChunkEntry* entry = it->second;
        size_t float_count = entry->size / sizeof(float);
        asset->positions.resize(float_count);

        file.seekg(entry->offset);
        file.read(reinterpret_cast<char*>(asset->positions.data()), entry->size);

        if (!file.good()) {
            return nullptr;
        }
    }

    // Load NORM chunk if present
    if (header.flags & Flags::HAS_NORMALS) {
        auto it = chunk_map.find(ChunkType::NORM);
        if (it != chunk_map.end()) {
            const ChunkEntry* entry = it->second;
            size_t count = entry->size / sizeof(OctNormal);
            asset->normals.resize(count);

            file.seekg(entry->offset);
            file.read(reinterpret_cast<char*>(asset->normals.data()), entry->size);

            if (!file.good()) {
                return nullptr;
            }
        }
    }

    // Load UVCO chunk if present
    if (header.flags & Flags::HAS_UVS) {
        auto it = chunk_map.find(ChunkType::UVCO);
        if (it != chunk_map.end()) {
            const ChunkEntry* entry = it->second;
            size_t count = entry->size / sizeof(uint16_t);
            asset->uvs.resize(count);

            file.seekg(entry->offset);
            file.read(reinterpret_cast<char*>(asset->uvs.data()), entry->size);

            if (!file.good()) {
                return nullptr;
            }
        }
    }

    // Load INDX chunk
    {
        auto it = chunk_map.find(ChunkType::INDX);
        if (it == chunk_map.end()) {
            return nullptr;  // INDX is required
        }

        const ChunkEntry* entry = it->second;

        // Indices are stored as uint32_t in the current implementation
        size_t count = entry->size / sizeof(uint32_t);
        asset->indices.resize(count);

        file.seekg(entry->offset);
        file.read(reinterpret_cast<char*>(asset->indices.data()), entry->size);

        if (!file.good()) {
            return nullptr;
        }
    }

    // Load MSLT chunk
    {
        auto it = chunk_map.find(ChunkType::MSLT);
        if (it == chunk_map.end()) {
            return nullptr;  // MSLT is required
        }

        const ChunkEntry* entry = it->second;
        size_t count = entry->size / sizeof(MeshletDescriptor);
        asset->meshlets.resize(count);

        file.seekg(entry->offset);
        file.read(reinterpret_cast<char*>(asset->meshlets.data()), entry->size);

        if (!file.good()) {
            return nullptr;
        }
    }

    // Load BVOL chunk
    {
        auto it = chunk_map.find(ChunkType::BVOL);
        if (it != chunk_map.end()) {
            const ChunkEntry* entry = it->second;
            size_t count = entry->size / sizeof(BoundingSphere);
            asset->meshlet_bounds.resize(count);

            file.seekg(entry->offset);
            file.read(reinterpret_cast<char*>(asset->meshlet_bounds.data()), entry->size);

            if (!file.good()) {
                return nullptr;
            }
        }
    }

    // Load CONE chunk
    {
        auto it = chunk_map.find(ChunkType::CONE);
        if (it != chunk_map.end()) {
            const ChunkEntry* entry = it->second;
            size_t count = entry->size / sizeof(NormalCone);
            asset->meshlet_cones.resize(count);

            file.seekg(entry->offset);
            file.read(reinterpret_cast<char*>(asset->meshlet_cones.data()), entry->size);

            if (!file.good()) {
                return nullptr;
            }
        }
    }

    // Load CLST chunk
    {
        auto it = chunk_map.find(ChunkType::CLST);
        if (it != chunk_map.end()) {
            const ChunkEntry* entry = it->second;
            size_t count = entry->size / sizeof(ClusterNode);
            asset->clusters.resize(count);

            file.seekg(entry->offset);
            file.read(reinterpret_cast<char*>(asset->clusters.data()), entry->size);

            if (!file.good()) {
                return nullptr;
            }
        }
    }

    return asset;
}

bool validate_vgeo(const std::string& path, std::string& out_error) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        out_error = "Cannot open file";
        return false;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    if (file_size < sizeof(FileHeader)) {
        out_error = "File too small for header";
        return false;
    }

    // Read and validate header
    FileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic != VGEO_MAGIC) {
        out_error = "Invalid magic number";
        return false;
    }

    if (header.version_major != VGEO_VERSION_MAJOR) {
        out_error = "Unsupported major version";
        return false;
    }

    // Validate chunk directory fits
    size_t directory_end = sizeof(FileHeader) + header.chunk_count * sizeof(ChunkEntry);
    if (file_size < directory_end) {
        out_error = "File too small for chunk directory";
        return false;
    }

    // Read chunk directory
    std::vector<ChunkEntry> chunks(header.chunk_count);
    file.read(reinterpret_cast<char*>(chunks.data()), header.chunk_count * sizeof(ChunkEntry));

    // Validate each chunk
    bool has_vert = false;
    bool has_indx = false;
    bool has_mslt = false;

    for (size_t i = 0; i < chunks.size(); i++) {
        const ChunkEntry& chunk = chunks[i];

        // Check offset is within file
        if (chunk.offset >= file_size) {
            out_error = "Chunk " + std::to_string(i) + " offset beyond file end";
            return false;
        }

        // Check chunk data fits within file
        if (chunk.offset + chunk.size > file_size) {
            out_error = "Chunk " + std::to_string(i) + " extends beyond file end";
            return false;
        }

        // Check 16-byte alignment
        if (chunk.offset % 16 != 0) {
            out_error = "Chunk " + std::to_string(i) + " not 16-byte aligned";
            return false;
        }

        // Track required chunks
        if (chunk.type == ChunkType::VERT) has_vert = true;
        if (chunk.type == ChunkType::INDX) has_indx = true;
        if (chunk.type == ChunkType::MSLT) has_mslt = true;
    }

    // Check required chunks are present
    if (!has_vert) {
        out_error = "Missing required VERT chunk";
        return false;
    }
    if (!has_indx) {
        out_error = "Missing required INDX chunk";
        return false;
    }
    if (!has_mslt) {
        out_error = "Missing required MSLT chunk";
        return false;
    }

    // Validate counts
    if (header.meshlet_count == 0 && header.vertex_count > 0) {
        out_error = "Has vertices but no meshlets";
        return false;
    }

    out_error.clear();
    return true;
}

} // namespace vgeo
