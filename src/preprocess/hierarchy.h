#pragma once

#include "meshlet_gen.h"
#include "vgeo_format.h"
#include <vector>

namespace vgeo {

// Hierarchy building parameters
struct HierarchyParams {
    uint32_t meshlets_per_cluster = 8;  // Group size
    float simplification_ratio = 0.5f;   // Target reduction per level
    float error_threshold = 1.0f;        // Screen-space pixels
};

// Result of hierarchy building
struct HierarchyData {
    std::vector<ClusterNode> clusters;
    std::vector<BoundingSphere> cluster_bounds;
    uint32_t root_cluster;
    uint32_t lod_levels;
};

// Build cluster hierarchy from meshlets
bool build_hierarchy(
    const MeshletData& meshlets,
    const RawMesh& mesh,
    const HierarchyParams& params,
    HierarchyData& out_hierarchy
);

} // namespace vgeo
