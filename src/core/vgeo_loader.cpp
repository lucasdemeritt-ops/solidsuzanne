// VGEO File Loader
// Load .vgeo files into GPU-ready structures

#include "vgeo_loader.h"

#include <fstream>
#include <cstring>
#include <unordered_map>

namespace vgeo {

// Sanity cap: a hostile chunk_count would otherwise drive a huge allocation
static constexpr uint32_t MAX_CHUNK_COUNT = 1024;

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

    if (header.chunk_count > MAX_CHUNK_COUNT) {
        return nullptr;
    }

    // Read chunk directory
    if (file_size < sizeof(FileHeader) +
            static_cast<uint64_t>(header.chunk_count) * sizeof(ChunkEntry)) {
        return nullptr;
    }

    std::vector<ChunkEntry> chunks(header.chunk_count);
    file.read(reinterpret_cast<char*>(chunks.data()), header.chunk_count * sizeof(ChunkEntry));
    if (!file.good()) {
        return nullptr;
    }

    // Build chunk map for easy lookup
    std::unordered_map<uint32_t, const ChunkEntry*> chunk_map;
    for (const auto& chunk : chunks) {
        chunk_map[chunk.type] = &chunk;
    }

    // Create asset
    auto asset = std::make_unique<VGeoAsset>();
    asset->header = header;

    // Read a chunk's payload into a vector of its element type.
    // Bounds are checked in 64-bit so offset+size cannot wrap, and only
    // whole elements are read so a truncated chunk size cannot overflow
    // the vector's allocation.
    auto read_chunk_vec = [&](uint32_t type, auto& vec, bool required) -> bool {
        auto it = chunk_map.find(type);
        if (it == chunk_map.end()) {
            return !required;
        }

        const ChunkEntry* entry = it->second;
        if (static_cast<uint64_t>(entry->offset) + entry->size > file_size) {
            return false;
        }

        using VecT = std::remove_reference_t<decltype(vec)>;
        using ElemT = typename VecT::value_type;
        size_t count = entry->size / sizeof(ElemT);
        vec.resize(count);
        if (count == 0) {
            return true;
        }

        file.seekg(entry->offset);
        file.read(reinterpret_cast<char*>(vec.data()), count * sizeof(ElemT));
        return file.good();
    };

    // Required chunks must also actually be present
    if (chunk_map.find(ChunkType::VERT) == chunk_map.end()) return nullptr;
    if (chunk_map.find(ChunkType::INDX) == chunk_map.end()) return nullptr;
    if (chunk_map.find(ChunkType::MSLT) == chunk_map.end()) return nullptr;

    if (!read_chunk_vec(ChunkType::VERT, asset->positions, true)) return nullptr;
    if (!read_chunk_vec(ChunkType::INDX, asset->indices, true)) return nullptr;
    if (!read_chunk_vec(ChunkType::MSLT, asset->meshlets, true)) return nullptr;

    if (header.flags & Flags::HAS_NORMALS) {
        if (!read_chunk_vec(ChunkType::NORM, asset->normals, false)) return nullptr;
    }
    if (header.flags & Flags::HAS_UVS) {
        if (!read_chunk_vec(ChunkType::UVCO, asset->uvs, false)) return nullptr;
    }
    if (!read_chunk_vec(ChunkType::BVOL, asset->meshlet_bounds, false)) return nullptr;
    if (!read_chunk_vec(ChunkType::CONE, asset->meshlet_cones, false)) return nullptr;
    if (!read_chunk_vec(ChunkType::CLST, asset->clusters, false)) return nullptr;
    if (!read_chunk_vec(ChunkType::CBND, asset->cluster_bounds, false)) return nullptr;

    // ========================================================================
    // Cross-validate loaded data so a corrupt or hostile file cannot drive
    // the renderer out of bounds.
    // ========================================================================

    // Header counts must match the chunk payloads
    if (asset->positions.size() != static_cast<uint64_t>(header.vertex_count) * 3) {
        return nullptr;
    }
    if (asset->meshlets.size() != header.meshlet_count) {
        return nullptr;
    }
    if (!asset->clusters.empty() && asset->clusters.size() != header.cluster_count) {
        return nullptr;
    }

    // Every index must reference a valid vertex
    for (uint32_t idx : asset->indices) {
        if (idx >= header.vertex_count) {
            return nullptr;
        }
    }

    // Cluster graph references must stay in range
    const uint64_t cluster_count = asset->clusters.size();
    const uint64_t meshlet_count = asset->meshlets.size();
    for (const ClusterNode& node : asset->clusters) {
        if (node.child_count != 0 &&
            static_cast<uint64_t>(node.child_start) + node.child_count > cluster_count) {
            return nullptr;
        }
        if (static_cast<uint64_t>(node.meshlet_start) + node.meshlet_count > meshlet_count) {
            return nullptr;
        }
        if (node.parent_id != INVALID_ID && node.parent_id >= cluster_count) {
            return nullptr;
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

    if (header.chunk_count > MAX_CHUNK_COUNT) {
        out_error = "Unreasonable chunk count";
        return false;
    }

    // Validate chunk directory fits
    uint64_t directory_end = sizeof(FileHeader) +
        static_cast<uint64_t>(header.chunk_count) * sizeof(ChunkEntry);
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

        // Check chunk data fits within file (64-bit so offset+size can't wrap)
        if (static_cast<uint64_t>(chunk.offset) + chunk.size > file_size) {
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
