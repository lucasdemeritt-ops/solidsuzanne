// VGEO File Writer
// Export meshlet data to .vgeo format

#include "vgeo_writer.h"
#include "vgeo_format.h"

#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>

namespace vgeo {

// Align offset to specified boundary
static uint32_t align_offset(uint32_t offset, uint32_t alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

// Encode normal to octahedral representation
static OctNormal encode_octahedral(float nx, float ny, float nz) {
    // Normalize
    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) {
        nx /= len;
        ny /= len;
        nz /= len;
    } else {
        nx = 0.0f;
        ny = 1.0f;
        nz = 0.0f;
    }

    // Project to octahedron
    float inv_l1 = 1.0f / (std::abs(nx) + std::abs(ny) + std::abs(nz));
    float ox = nx * inv_l1;
    float oy = ny * inv_l1;

    // Wrap lower hemisphere
    if (nz < 0.0f) {
        float tx = (1.0f - std::abs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        float ty = (1.0f - std::abs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
        ox = tx;
        oy = ty;
    }

    // Convert to snorm16
    OctNormal result;
    result.x = static_cast<int16_t>(std::clamp(ox * 32767.0f, -32767.0f, 32767.0f));
    result.y = static_cast<int16_t>(std::clamp(oy * 32767.0f, -32767.0f, 32767.0f));

    return result;
}

// Convert float to half-float (IEEE 754 binary16)
static uint16_t float_to_half(float value) {
    uint32_t f;
    std::memcpy(&f, &value, sizeof(f));
    uint32_t sign = (f >> 16) & 0x8000;
    int32_t exp = ((f >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = f & 0x7FFFFF;

    if (exp <= 0) {
        // Denormal or zero
        if (exp < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa = (mantissa | 0x800000) >> (1 - exp);
        return static_cast<uint16_t>(sign | (mantissa >> 13));
    } else if (exp >= 31) {
        // Infinity or NaN
        return static_cast<uint16_t>(sign | 0x7C00 | (mantissa ? 0x200 : 0));
    }

    return static_cast<uint16_t>(sign | (exp << 10) | (mantissa >> 13));
}

// Compute mesh bounds
static AABB compute_mesh_bounds(const RawMesh& mesh) {
    AABB bounds;

    if (mesh.positions.empty()) {
        bounds.min[0] = bounds.min[1] = bounds.min[2] = 0.0f;
        bounds.max[0] = bounds.max[1] = bounds.max[2] = 0.0f;
        return bounds;
    }

    bounds.min[0] = bounds.min[1] = bounds.min[2] = std::numeric_limits<float>::max();
    bounds.max[0] = bounds.max[1] = bounds.max[2] = std::numeric_limits<float>::lowest();

    for (size_t i = 0; i < mesh.positions.size() / 3; i++) {
        bounds.min[0] = std::min(bounds.min[0], mesh.positions[i * 3 + 0]);
        bounds.min[1] = std::min(bounds.min[1], mesh.positions[i * 3 + 1]);
        bounds.min[2] = std::min(bounds.min[2], mesh.positions[i * 3 + 2]);
        bounds.max[0] = std::max(bounds.max[0], mesh.positions[i * 3 + 0]);
        bounds.max[1] = std::max(bounds.max[1], mesh.positions[i * 3 + 1]);
        bounds.max[2] = std::max(bounds.max[2], mesh.positions[i * 3 + 2]);
    }

    return bounds;
}

bool write_vgeo(
    const std::string& path,
    const RawMesh& mesh,
    const MeshletData& meshlets,
    const HierarchyData& hierarchy,
    const WriteParams& params
) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    // Determine flags
    uint32_t flags = 0;
    if (params.compress) {
        flags |= Flags::COMPRESSED;
    }
    if (params.quantize_positions) {
        flags |= Flags::QUANTIZED_POS;
    }
    if (!mesh.normals.empty()) {
        flags |= Flags::HAS_NORMALS;
    }
    if (!mesh.uvs.empty()) {
        flags |= Flags::HAS_UVS;
    }

    // The INDX chunk is always written as 32-bit indices (see below), so the
    // INDEX_16BIT flag is intentionally not set. Setting it without writing
    // 16-bit data would mislead any consumer that binds the buffer as UINT16.

    // Count chunks
    uint32_t chunk_count = 0;
    chunk_count++;  // VERT
    if (flags & Flags::HAS_NORMALS) chunk_count++;  // NORM
    if (flags & Flags::HAS_UVS) chunk_count++;      // UVCO
    chunk_count++;  // INDX
    chunk_count++;  // MSLT
    chunk_count++;  // CLST
    chunk_count++;  // BVOL (meshlet bounds)
    if (!hierarchy.cluster_bounds.empty()) chunk_count++;  // CBND (cluster bounds)
    chunk_count++;  // CONE

    // Calculate sizes
    uint32_t vertex_count = mesh.vertex_count();
    uint32_t index_count = static_cast<uint32_t>(meshlets.local_indices.size());

    // Prepare chunk data
    std::vector<std::vector<uint8_t>> chunk_data(chunk_count);
    std::vector<uint32_t> chunk_types(chunk_count);

    uint32_t chunk_idx = 0;

    // VERT chunk - vertex positions (float32 x 3 per vertex)
    {
        chunk_types[chunk_idx] = ChunkType::VERT;
        auto& data = chunk_data[chunk_idx];
        data.resize(vertex_count * 3 * sizeof(float));
        memcpy(data.data(), mesh.positions.data(), data.size());
        chunk_idx++;
    }

    // NORM chunk - octahedral-encoded normals
    if (flags & Flags::HAS_NORMALS) {
        chunk_types[chunk_idx] = ChunkType::NORM;
        auto& data = chunk_data[chunk_idx];
        data.resize(vertex_count * sizeof(OctNormal));

        OctNormal* normals_out = reinterpret_cast<OctNormal*>(data.data());
        for (uint32_t i = 0; i < vertex_count; i++) {
            normals_out[i] = encode_octahedral(
                mesh.normals[i * 3 + 0],
                mesh.normals[i * 3 + 1],
                mesh.normals[i * 3 + 2]
            );
        }
        chunk_idx++;
    }

    // UVCO chunk - half-float UVs
    if (flags & Flags::HAS_UVS) {
        chunk_types[chunk_idx] = ChunkType::UVCO;
        auto& data = chunk_data[chunk_idx];
        data.resize(vertex_count * 2 * sizeof(uint16_t));

        uint16_t* uvs_out = reinterpret_cast<uint16_t*>(data.data());
        for (uint32_t i = 0; i < vertex_count; i++) {
            uvs_out[i * 2 + 0] = float_to_half(mesh.uvs[i * 2 + 0]);
            uvs_out[i * 2 + 1] = float_to_half(mesh.uvs[i * 2 + 1]);
        }
        chunk_idx++;
    }

    // INDX chunk - global vertex indices (expanded from meshlet local indices)
    {
        chunk_types[chunk_idx] = ChunkType::INDX;
        auto& data = chunk_data[chunk_idx];

        // Expand local indices to global indices using vertex_indices mapping
        // For each meshlet, local_indices[i] indexes into vertex_indices[meshlet.vertex_offset + local_index]
        std::vector<uint32_t> global_indices;
        global_indices.reserve(index_count);

        for (const auto& meshlet : meshlets.meshlets) {
            for (uint32_t t = 0; t < meshlet.triangle_count; t++) {
                for (uint32_t v = 0; v < 3; v++) {
                    uint32_t local_idx_offset = meshlet.index_offset + t * 3 + v;
                    uint8_t local_idx = meshlets.local_indices[local_idx_offset];
                    uint32_t global_idx = meshlets.vertex_indices[meshlet.vertex_offset + local_idx];
                    global_indices.push_back(global_idx);
                }
            }
        }

        data.resize(global_indices.size() * sizeof(uint32_t));
        memcpy(data.data(), global_indices.data(), data.size());
        chunk_idx++;
    }

    // MSLT chunk - meshlet descriptors
    {
        chunk_types[chunk_idx] = ChunkType::MSLT;
        auto& data = chunk_data[chunk_idx];
        uint32_t meshlet_count = static_cast<uint32_t>(meshlets.meshlets.size());
        data.resize(meshlet_count * sizeof(MeshletDescriptor));
        memcpy(data.data(), meshlets.meshlets.data(), data.size());
        chunk_idx++;
    }

    // CLST chunk - cluster hierarchy
    {
        chunk_types[chunk_idx] = ChunkType::CLST;
        auto& data = chunk_data[chunk_idx];
        uint32_t cluster_count = static_cast<uint32_t>(hierarchy.clusters.size());
        data.resize(cluster_count * sizeof(ClusterNode));
        memcpy(data.data(), hierarchy.clusters.data(), data.size());
        chunk_idx++;
    }

    // BVOL chunk - meshlet bounding spheres
    {
        chunk_types[chunk_idx] = ChunkType::BVOL;
        auto& data = chunk_data[chunk_idx];
        uint32_t bounds_count = static_cast<uint32_t>(meshlets.bounds.size());
        data.resize(bounds_count * sizeof(BoundingSphere));
        memcpy(data.data(), meshlets.bounds.data(), data.size());
        chunk_idx++;
    }

    // CBND chunk - cluster bounding spheres (for LOD hierarchy traversal)
    if (!hierarchy.cluster_bounds.empty()) {
        chunk_types[chunk_idx] = ChunkType::CBND;
        auto& data = chunk_data[chunk_idx];
        uint32_t bounds_count = static_cast<uint32_t>(hierarchy.cluster_bounds.size());
        data.resize(bounds_count * sizeof(BoundingSphere));
        memcpy(data.data(), hierarchy.cluster_bounds.data(), data.size());
        chunk_idx++;
    }

    // CONE chunk - normal cones
    {
        chunk_types[chunk_idx] = ChunkType::CONE;
        auto& data = chunk_data[chunk_idx];
        uint32_t cone_count = static_cast<uint32_t>(meshlets.cones.size());
        data.resize(cone_count * sizeof(NormalCone));
        memcpy(data.data(), meshlets.cones.data(), data.size());
        chunk_idx++;
    }

    // Build chunk directory
    std::vector<ChunkEntry> chunk_entries(chunk_count);

    // Calculate offsets
    uint32_t header_size = sizeof(FileHeader);
    uint32_t directory_size = chunk_count * sizeof(ChunkEntry);
    uint32_t data_offset = align_offset(header_size + directory_size, 16);

    for (uint32_t i = 0; i < chunk_count; i++) {
        chunk_entries[i].type = chunk_types[i];
        chunk_entries[i].offset = data_offset;
        chunk_entries[i].size = static_cast<uint32_t>(chunk_data[i].size());
        chunk_entries[i].compressed_size = 0;  // No compression for now

        // Next chunk starts after this one, aligned to 16 bytes
        data_offset = align_offset(data_offset + chunk_entries[i].size, 16);
    }

    // Build header
    FileHeader header = {};
    header.magic = VGEO_MAGIC;
    header.version_major = VGEO_VERSION_MAJOR;
    header.version_minor = VGEO_VERSION_MINOR;
    header.flags = flags;
    header.meshlet_count = static_cast<uint32_t>(meshlets.meshlets.size());
    header.cluster_count = static_cast<uint32_t>(hierarchy.clusters.size());
    header.vertex_count = vertex_count;
    header.index_count = index_count;
    header.chunk_count = chunk_count;
    header.bounds = compute_mesh_bounds(mesh);
    memset(header.reserved, 0, sizeof(header.reserved));

    // Write file
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(chunk_entries.data()), directory_size);

    // Pad to first chunk
    uint32_t current_offset = header_size + directory_size;
    while (current_offset < chunk_entries[0].offset) {
        char zero = 0;
        file.write(&zero, 1);
        current_offset++;
    }

    // Write chunks with padding
    for (uint32_t i = 0; i < chunk_count; i++) {
        file.write(reinterpret_cast<const char*>(chunk_data[i].data()), chunk_data[i].size());
        current_offset += static_cast<uint32_t>(chunk_data[i].size());

        // Pad to next chunk alignment
        if (i + 1 < chunk_count) {
            while (current_offset < chunk_entries[i + 1].offset) {
                char zero = 0;
                file.write(&zero, 1);
                current_offset++;
            }
        }
    }

    return true;
}

} // namespace vgeo
