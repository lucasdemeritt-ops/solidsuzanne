#pragma once

#include "meshlet_gen.h"
#include "vgeo_format.h"
#include <vector>
#include <cstdint>

namespace vgeo {

// Hierarchy building parameters
struct HierarchyParams {
    uint32_t meshlets_per_cluster = 8;   // Target meshlets per leaf cluster
    uint32_t branching_factor = 4;       // Target children per parent node (4-8 recommended)
    uint32_t target_lod_levels = 0;      // 0 = auto-calculate based on meshlet count
    float simplification_ratio = 0.5f;   // Target reduction per level
    float error_threshold = 1.0f;        // Base screen-space error threshold (pixels)
    float error_scale_per_level = 2.0f;  // Error multiplier between adjacent levels
    bool use_spatial_grouping = true;    // Use spatial proximity for grouping
};

// Result of hierarchy building
struct HierarchyData {
    std::vector<ClusterNode> clusters;
    std::vector<BoundingSphere> cluster_bounds;
    std::vector<uint32_t> root_clusters;  // Multiple roots possible for large meshes
    uint32_t lod_levels;

    // Statistics
    uint32_t total_leaf_clusters;
    uint32_t total_internal_clusters;
};

// Spatial partitioning helper for grouping nearby meshlets/clusters
struct SpatialGroup {
    std::vector<uint32_t> indices;  // Indices of items in this group
    BoundingSphere bounds{};        // Combined bounding sphere
    float error = 0.0f;             // Combined error metric
};

// Build cluster hierarchy from meshlets using full multi-level DAG.
// Reorders meshlets (descriptors, bounds, cones) so that each leaf cluster
// references a contiguous meshlet range, and assigns each meshlet's cluster_id.
bool build_hierarchy(
    MeshletData& meshlets,
    const RawMesh& mesh,
    const HierarchyParams& params,
    HierarchyData& out_hierarchy
);

// Helper functions for hierarchy construction

// Compute bounding sphere that encloses multiple spheres
BoundingSphere merge_bounding_spheres(
    const BoundingSphere* spheres,
    uint32_t count
);

// Compute bounding sphere from a list with indirect indices
BoundingSphere merge_bounding_spheres_indexed(
    const std::vector<BoundingSphere>& spheres,
    const std::vector<uint32_t>& indices
);

// Group items spatially using proximity-based clustering
// Returns groups of indices, each group will become children of a parent cluster
std::vector<SpatialGroup> group_spatially(
    const std::vector<BoundingSphere>& bounds,
    const std::vector<uint32_t>& indices,
    uint32_t target_group_size,
    uint32_t branching_factor
);

// Compute error metric for a cluster at a given level
float compute_level_error(
    float child_max_error,
    uint32_t lod_level,
    float error_scale
);

// Calculate optimal number of LOD levels for a given meshlet count
uint32_t calculate_lod_levels(
    uint32_t meshlet_count,
    uint32_t meshlets_per_cluster,
    uint32_t branching_factor
);

} // namespace vgeo
