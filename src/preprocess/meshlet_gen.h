#pragma once

#include "mesh_import.h"
#include "vgeo_format.h"
#include <vector>

namespace vgeo {

// Meshlet generation parameters
struct MeshletParams {
    uint32_t max_vertices = 64;
    uint32_t max_triangles = 126;
    float cone_weight = 0.5f;  // Normal cone optimization weight
};

// Result of meshlet generation
struct MeshletData {
    std::vector<uint32_t> vertex_indices;  // Global vertex indices
    std::vector<uint8_t> local_indices;    // Local triangle indices (3 per tri)
    std::vector<MeshletDescriptor> meshlets;
    std::vector<BoundingSphere> bounds;
    std::vector<NormalCone> cones;
};

// Generate meshlets from raw mesh
bool generate_meshlets(
    const RawMesh& mesh,
    const MeshletParams& params,
    MeshletData& out_data
);

} // namespace vgeo
