#pragma once

#include <vector>
#include <string>
#include <cstdint>

namespace vgeo {

struct RawMesh {
    std::vector<float> positions;    // x,y,z per vertex
    std::vector<float> normals;      // x,y,z per vertex
    std::vector<float> uvs;          // u,v per vertex
    std::vector<uint32_t> indices;   // 3 per triangle

    uint32_t vertex_count() const { return positions.size() / 3; }
    uint32_t triangle_count() const { return indices.size() / 3; }
};

// Load mesh from file
// Supported: .obj, .gltf, .glb
bool load_mesh(const std::string& path, RawMesh& out_mesh);

} // namespace vgeo
