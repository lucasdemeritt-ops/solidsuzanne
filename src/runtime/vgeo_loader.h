#pragma once

#include "vgeo_format.h"
#include <string>
#include <vector>
#include <memory>

namespace vgeo {

// Loaded VGEO asset ready for GPU upload
struct VGeoAsset {
    FileHeader header;

    // Vertex data
    std::vector<float> positions;
    std::vector<OctNormal> normals;
    std::vector<uint16_t> uvs;  // half-float

    // Index data
    std::vector<uint32_t> indices;

    // Meshlet data
    std::vector<MeshletDescriptor> meshlets;
    std::vector<BoundingSphere> meshlet_bounds;
    std::vector<NormalCone> meshlet_cones;

    // Hierarchy
    std::vector<ClusterNode> clusters;
};

// Load .vgeo file from disk
std::unique_ptr<VGeoAsset> load_vgeo(const std::string& path);

// Validate .vgeo file without full load
bool validate_vgeo(const std::string& path, std::string& out_error);

} // namespace vgeo
