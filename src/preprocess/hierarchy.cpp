// VGEO Cluster Hierarchy
// Build LOD hierarchy from meshlets
// This is a simple 2-level implementation (meshlets -> root)
// TODO: Implement full multi-level hierarchy with simplification

#include "hierarchy.h"

#include <algorithm>
#include <cmath>

namespace vgeo {

// Compute bounding sphere that encloses multiple spheres
static BoundingSphere merge_bounding_spheres(
    const std::vector<BoundingSphere>& spheres,
    uint32_t start,
    uint32_t count
) {
    BoundingSphere result = {};

    if (count == 0) {
        return result;
    }

    if (count == 1) {
        return spheres[start];
    }

    // First, compute the centroid of all sphere centers
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        const BoundingSphere& s = spheres[start + i];
        cx += s.center[0];
        cy += s.center[1];
        cz += s.center[2];
    }
    float inv_count = 1.0f / static_cast<float>(count);
    cx *= inv_count;
    cy *= inv_count;
    cz *= inv_count;

    result.center[0] = cx;
    result.center[1] = cy;
    result.center[2] = cz;

    // Find the maximum distance from centroid to any sphere's far edge
    float max_dist = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        const BoundingSphere& s = spheres[start + i];
        float dx = s.center[0] - cx;
        float dy = s.center[1] - cy;
        float dz = s.center[2] - cz;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz) + s.radius;
        max_dist = std::max(max_dist, dist);
    }

    result.radius = max_dist;
    return result;
}

// Compute error metric for a cluster
// This is a simplified metric - proper implementation would use quadric error
static float compute_cluster_error(
    const MeshletData& meshlets,
    const RawMesh& mesh,
    uint32_t meshlet_start,
    uint32_t meshlet_count
) {
    if (meshlet_count == 0) {
        return 0.0f;
    }

    // Use the maximum bounding sphere radius as a simple error proxy
    // Better: use surface area or actual geometric simplification error
    float max_radius = 0.0f;
    for (uint32_t i = 0; i < meshlet_count; i++) {
        max_radius = std::max(max_radius, meshlets.bounds[meshlet_start + i].radius);
    }

    return max_radius;
}

bool build_hierarchy(
    const MeshletData& meshlets,
    const RawMesh& mesh,
    const HierarchyParams& params,
    HierarchyData& out_hierarchy
) {
    out_hierarchy.clusters.clear();
    out_hierarchy.cluster_bounds.clear();
    out_hierarchy.root_cluster = 0;
    out_hierarchy.lod_levels = 1;

    const uint32_t meshlet_count = static_cast<uint32_t>(meshlets.meshlets.size());

    if (meshlet_count == 0) {
        // Create a dummy root cluster for empty mesh
        ClusterNode root;
        root.parent_id = INVALID_ID;
        root.child_start = 0;
        root.child_count = 0;
        root.meshlet_start = 0;
        root.meshlet_count = 0;
        root.error = 0.0f;
        root.parent_error = 0.0f;
        root.lod_level = 0;
        root.padding[0] = root.padding[1] = root.padding[2] = 0;

        out_hierarchy.clusters.push_back(root);
        out_hierarchy.cluster_bounds.push_back(BoundingSphere{});
        return true;
    }

    const uint32_t meshlets_per_cluster = params.meshlets_per_cluster;

    // Simple 2-level hierarchy:
    // Level 0: Leaf clusters (each containing a group of meshlets)
    // Level 1: Root cluster (containing all leaf clusters)

    // Calculate number of leaf clusters
    uint32_t leaf_cluster_count = (meshlet_count + meshlets_per_cluster - 1) / meshlets_per_cluster;

    // Reserve space
    out_hierarchy.clusters.reserve(leaf_cluster_count + 1);
    out_hierarchy.cluster_bounds.reserve(leaf_cluster_count + 1);

    // Create leaf clusters (indices 0 to leaf_cluster_count-1)
    for (uint32_t i = 0; i < leaf_cluster_count; i++) {
        uint32_t meshlet_start = i * meshlets_per_cluster;
        uint32_t count = std::min(meshlets_per_cluster, meshlet_count - meshlet_start);

        ClusterNode leaf;
        leaf.parent_id = leaf_cluster_count;  // Root will be at this index
        leaf.child_start = 0;
        leaf.child_count = 0;  // Leaf nodes have no children
        leaf.meshlet_start = meshlet_start;
        leaf.meshlet_count = count;
        leaf.error = compute_cluster_error(meshlets, mesh, meshlet_start, count);
        leaf.parent_error = 0.0f;  // Will be set when root is created
        leaf.lod_level = 0;  // Highest detail
        leaf.padding[0] = leaf.padding[1] = leaf.padding[2] = 0;

        out_hierarchy.clusters.push_back(leaf);

        // Compute bounds for this cluster
        BoundingSphere bounds = merge_bounding_spheres(
            meshlets.bounds,
            meshlet_start,
            count
        );
        out_hierarchy.cluster_bounds.push_back(bounds);
    }

    // Create root cluster
    ClusterNode root;
    root.parent_id = INVALID_ID;  // Root has no parent
    root.child_start = 0;
    root.child_count = leaf_cluster_count;
    root.meshlet_start = 0;
    root.meshlet_count = 0;  // Root doesn't directly own meshlets
    root.lod_level = 1;
    root.padding[0] = root.padding[1] = root.padding[2] = 0;

    // Compute root error (maximum of child errors, scaled up)
    float max_child_error = 0.0f;
    for (uint32_t i = 0; i < leaf_cluster_count; i++) {
        max_child_error = std::max(max_child_error, out_hierarchy.clusters[i].error);
    }
    root.error = max_child_error * 2.0f;  // Root represents a coarser LOD
    root.parent_error = root.error * 2.0f;  // For LOD cut calculation

    // Update parent_error for all leaf clusters
    for (uint32_t i = 0; i < leaf_cluster_count; i++) {
        out_hierarchy.clusters[i].parent_error = root.error;
    }

    out_hierarchy.clusters.push_back(root);
    out_hierarchy.root_cluster = leaf_cluster_count;

    // Compute root bounds (enclosing all leaf bounds)
    BoundingSphere root_bounds = merge_bounding_spheres(
        out_hierarchy.cluster_bounds,
        0,
        leaf_cluster_count
    );
    out_hierarchy.cluster_bounds.push_back(root_bounds);

    out_hierarchy.lod_levels = 2;

    return true;
}

} // namespace vgeo
