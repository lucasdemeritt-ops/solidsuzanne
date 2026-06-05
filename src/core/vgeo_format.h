#pragma once

#include <cstdint>
#include <array>

namespace vgeo {

// File format magic and version
constexpr uint32_t VGEO_MAGIC = 0x4F454756; // "VGEO" in little-endian
constexpr uint16_t VGEO_VERSION_MAJOR = 0;
constexpr uint16_t VGEO_VERSION_MINOR = 1;

// Chunk type identifiers
namespace ChunkType {
    constexpr uint32_t VERT = 0x54524556; // "VERT"
    constexpr uint32_t NORM = 0x4D524F4E; // "NORM"
    constexpr uint32_t UVCO = 0x4F435655; // "UVCO"
    constexpr uint32_t INDX = 0x58444E49; // "INDX"
    constexpr uint32_t MSLT = 0x544C534D; // "MSLT"
    constexpr uint32_t CLST = 0x54534C43; // "CLST"
    constexpr uint32_t BVOL = 0x4C4F5642; // "BVOL" - meshlet bounding spheres
    constexpr uint32_t CBND = 0x444E4243; // "CBND" - cluster bounding spheres
    constexpr uint32_t CONE = 0x454E4F43; // "CONE"
}

// Flags bitfield
namespace Flags {
    constexpr uint32_t COMPRESSED      = 1 << 0;
    constexpr uint32_t QUANTIZED_POS   = 1 << 1;
    constexpr uint32_t HAS_NORMALS     = 1 << 2;
    constexpr uint32_t HAS_UVS         = 1 << 3;
    constexpr uint32_t HAS_TANGENTS    = 1 << 4;
    constexpr uint32_t INDEX_16BIT     = 1 << 5;
}

// Axis-Aligned Bounding Box
struct AABB {
    float min[3];
    float max[3];
};

// File header (64 bytes, fixed size)
struct FileHeader {
    uint32_t magic;           // Must be VGEO_MAGIC
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t flags;
    uint32_t meshlet_count;
    uint32_t cluster_count;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t chunk_count;
    AABB bounds;
    uint8_t reserved[8];
};
static_assert(sizeof(FileHeader) == 64, "FileHeader must be 64 bytes");

// Chunk directory entry (16 bytes)
struct ChunkEntry {
    uint32_t type;            // ChunkType::*
    uint32_t offset;          // Byte offset from file start
    uint32_t size;            // Uncompressed size
    uint32_t compressed_size; // 0 if not compressed
};
static_assert(sizeof(ChunkEntry) == 16, "ChunkEntry must be 16 bytes");

// Meshlet descriptor
struct MeshletDescriptor {
    uint32_t vertex_offset;   // Offset into vertex buffer
    uint32_t vertex_count;    // Number of vertices
    uint32_t index_offset;    // Offset into index buffer
    uint32_t triangle_count;  // Number of triangles
    uint32_t cluster_id;      // Parent cluster in hierarchy
};

// Cluster node in hierarchy
struct ClusterNode {
    uint32_t parent_id;       // 0xFFFFFFFF if root
    uint32_t child_start;     // First child index
    uint32_t child_count;     // Number of children
    uint32_t meshlet_start;   // First meshlet (leaf nodes)
    uint32_t meshlet_count;   // Meshlets in this cluster
    float error;              // Geometric error metric
    float parent_error;       // Parent's error (for LOD cut)
    uint8_t lod_level;        // 0 = highest detail
    uint8_t padding[3];
};

// Bounding sphere for culling
struct BoundingSphere {
    float center[3];
    float radius;
};

// Normal cone for backface culling
struct NormalCone {
    int8_t axis[3];           // Normalized axis (snorm8)
    int8_t cos_angle;         // cos(aperture/2) as snorm8
};

// Octahedral-encoded normal
struct OctNormal {
    int16_t x, y;             // snorm16, z derived
};

// Validation
constexpr uint32_t INVALID_ID = 0xFFFFFFFF;

inline bool is_valid_header(const FileHeader& h) {
    return h.magic == VGEO_MAGIC &&
           h.version_major == VGEO_VERSION_MAJOR;
}

} // namespace vgeo
