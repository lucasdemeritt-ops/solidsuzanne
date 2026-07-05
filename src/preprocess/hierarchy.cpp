// VGEO Cluster Hierarchy
// Build multi-level LOD hierarchy (DAG) from meshlets
// Uses bottom-up construction with spatial grouping

#include "hierarchy.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>
#include <queue>

namespace vgeo {

// ============================================================================
// Bounding Sphere Utilities
// ============================================================================

BoundingSphere merge_bounding_spheres(
    const BoundingSphere* spheres,
    uint32_t count
) {
    BoundingSphere result = {};

    if (count == 0) {
        return result;
    }

    if (count == 1) {
        return spheres[0];
    }

    // First, compute the centroid of all sphere centers
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        const BoundingSphere& s = spheres[i];
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
        const BoundingSphere& s = spheres[i];
        float dx = s.center[0] - cx;
        float dy = s.center[1] - cy;
        float dz = s.center[2] - cz;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz) + s.radius;
        max_dist = std::max(max_dist, dist);
    }

    result.radius = max_dist;
    return result;
}

BoundingSphere merge_bounding_spheres_indexed(
    const std::vector<BoundingSphere>& spheres,
    const std::vector<uint32_t>& indices
) {
    if (indices.empty()) {
        return BoundingSphere{};
    }

    // Gather the spheres
    std::vector<BoundingSphere> selected;
    selected.reserve(indices.size());
    for (uint32_t idx : indices) {
        if (idx < spheres.size()) {
            selected.push_back(spheres[idx]);
        }
    }

    if (selected.empty()) {
        return BoundingSphere{};
    }

    return merge_bounding_spheres(selected.data(), static_cast<uint32_t>(selected.size()));
}

// ============================================================================
// Spatial Distance Calculations
// ============================================================================

// Compute squared distance between two sphere centers
static float sphere_distance_squared(const BoundingSphere& a, const BoundingSphere& b) {
    float dx = a.center[0] - b.center[0];
    float dy = a.center[1] - b.center[1];
    float dz = a.center[2] - b.center[2];
    return dx * dx + dy * dy + dz * dz;
}

// Compute overlap score (negative means overlapping, positive means gap)
static float sphere_gap(const BoundingSphere& a, const BoundingSphere& b) {
    float dist = std::sqrt(sphere_distance_squared(a, b));
    return dist - a.radius - b.radius;
}

// ============================================================================
// Spatial Grouping Algorithm
// ============================================================================

// Find the nearest ungrouped item to a given centroid
static uint32_t find_nearest(
    const std::vector<BoundingSphere>& bounds,
    const std::vector<uint32_t>& candidates,
    const std::vector<bool>& grouped,
    const BoundingSphere& centroid
) {
    uint32_t best_idx = INVALID_ID;
    float best_dist = std::numeric_limits<float>::max();

    for (uint32_t idx : candidates) {
        if (grouped[idx]) continue;

        float dist_sq = sphere_distance_squared(bounds[idx], centroid);
        if (dist_sq < best_dist) {
            best_dist = dist_sq;
            best_idx = idx;
        }
    }

    return best_idx;
}

// Find the pair of ungrouped items with minimum distance
static void find_nearest_pair(
    const std::vector<BoundingSphere>& bounds,
    const std::vector<uint32_t>& candidates,
    const std::vector<bool>& grouped,
    uint32_t& out_a,
    uint32_t& out_b
) {
    out_a = INVALID_ID;
    out_b = INVALID_ID;
    float best_dist = std::numeric_limits<float>::max();

    for (size_t i = 0; i < candidates.size(); i++) {
        uint32_t idx_a = candidates[i];
        if (grouped[idx_a]) continue;

        for (size_t j = i + 1; j < candidates.size(); j++) {
            uint32_t idx_b = candidates[j];
            if (grouped[idx_b]) continue;

            float dist_sq = sphere_distance_squared(bounds[idx_a], bounds[idx_b]);
            if (dist_sq < best_dist) {
                best_dist = dist_sq;
                out_a = idx_a;
                out_b = idx_b;
            }
        }
    }
}

std::vector<SpatialGroup> group_spatially(
    const std::vector<BoundingSphere>& bounds,
    const std::vector<uint32_t>& indices,
    uint32_t target_group_size,
    uint32_t branching_factor
) {
    std::vector<SpatialGroup> groups;

    if (indices.empty()) {
        return groups;
    }

    // Guard against division by zero / degenerate grouping below
    target_group_size = std::max(target_group_size, 1u);
    branching_factor = std::max(branching_factor, 1u);

    // Special case: few items, just make one group
    if (indices.size() <= branching_factor) {
        SpatialGroup group;
        group.indices = indices;
        group.bounds = merge_bounding_spheres_indexed(bounds, indices);
        group.error = 0.0f;
        groups.push_back(std::move(group));
        return groups;
    }

    // Track which items have been grouped
    std::vector<bool> grouped(bounds.size(), false);
    uint32_t remaining = static_cast<uint32_t>(indices.size());

    // Calculate number of groups needed
    uint32_t num_groups = (remaining + target_group_size - 1) / target_group_size;
    groups.reserve(num_groups);

    // Greedy spatial clustering
    while (remaining > 0) {
        SpatialGroup new_group;

        // Find the pair of closest ungrouped items to seed this group
        uint32_t seed_a, seed_b;
        find_nearest_pair(bounds, indices, grouped, seed_a, seed_b);

        if (seed_a == INVALID_ID) {
            // Only one item left
            for (uint32_t idx : indices) {
                if (!grouped[idx]) {
                    new_group.indices.push_back(idx);
                    grouped[idx] = true;
                    remaining--;
                    break;
                }
            }
        } else {
            // Start with the closest pair
            new_group.indices.push_back(seed_a);
            new_group.indices.push_back(seed_b);
            grouped[seed_a] = true;
            grouped[seed_b] = true;
            remaining -= 2;

            // Compute initial group bounds
            new_group.bounds = merge_bounding_spheres_indexed(bounds, new_group.indices);

            // Grow the group by adding nearest items
            uint32_t max_size = std::min(target_group_size, remaining + static_cast<uint32_t>(new_group.indices.size()));

            while (new_group.indices.size() < max_size && remaining > 0) {
                // Find nearest ungrouped item to current group centroid
                uint32_t nearest = find_nearest(bounds, indices, grouped, new_group.bounds);
                if (nearest == INVALID_ID) break;

                // Check if adding this item would make the group too spread out
                // (optional heuristic to prevent elongated groups)
                float gap = sphere_gap(bounds[nearest], new_group.bounds);
                float threshold = new_group.bounds.radius * 2.0f;  // Allow some spread

                if (gap > threshold && new_group.indices.size() >= branching_factor) {
                    // Item is too far, start a new group instead
                    break;
                }

                new_group.indices.push_back(nearest);
                grouped[nearest] = true;
                remaining--;

                // Update bounds
                new_group.bounds = merge_bounding_spheres_indexed(bounds, new_group.indices);
            }
        }

        // Finalize group bounds
        new_group.bounds = merge_bounding_spheres_indexed(bounds, new_group.indices);
        groups.push_back(std::move(new_group));
    }

    return groups;
}

// ============================================================================
// Error Metric Computation
// ============================================================================

// Compute error for a leaf cluster based on meshlet geometric properties
static float compute_leaf_cluster_error(
    const MeshletData& meshlets,
    const std::vector<uint32_t>& meshlet_indices
) {
    if (meshlet_indices.empty()) {
        return 0.0f;
    }

    // Use max bounding sphere radius as error proxy
    // This represents the maximum geometric detail size in the cluster
    float max_radius = 0.0f;
    for (uint32_t idx : meshlet_indices) {
        if (idx < meshlets.bounds.size()) {
            max_radius = std::max(max_radius, meshlets.bounds[idx].radius);
        }
    }

    // Scale by a factor to convert to screen-space error proxy
    // This is a simplified metric; a proper implementation would use
    // actual simplification error from mesh decimation
    return max_radius * 0.1f;  // Tune this factor based on testing
}

float compute_level_error(
    float child_max_error,
    uint32_t lod_level,
    float error_scale
) {
    // Parent error = max child error * scale factor
    // This represents the error introduced by switching from children to parent
    (void)lod_level;  // lod_level can be used for non-linear scaling if needed
    return child_max_error * error_scale;
}

// ============================================================================
// LOD Level Calculation
// ============================================================================

uint32_t calculate_lod_levels(
    uint32_t meshlet_count,
    uint32_t meshlets_per_cluster,
    uint32_t branching_factor
) {
    if (meshlet_count == 0) {
        return 1;
    }

    // meshlets_per_cluster == 0 would divide by zero below;
    // branching_factor < 2 would make the reduction loop spin forever
    meshlets_per_cluster = std::max(meshlets_per_cluster, 1u);
    branching_factor = std::max(branching_factor, 2u);

    // Calculate leaf cluster count
    uint32_t leaf_count = (meshlet_count + meshlets_per_cluster - 1) / meshlets_per_cluster;

    if (leaf_count <= 1) {
        return 1;
    }

    // Calculate levels needed to reduce to a single root (or small number of roots)
    // levels = ceil(log_branching(leaf_count))
    uint32_t levels = 1;  // Start with leaf level
    uint32_t current_count = leaf_count;

    while (current_count > branching_factor) {
        current_count = (current_count + branching_factor - 1) / branching_factor;
        levels++;
    }

    // Add one more for the final root level
    levels++;

    return std::min(levels, 16u);  // Cap at 16 levels
}

// ============================================================================
// Helper structure for reordering clusters
// ============================================================================

struct ClusterInfo {
    ClusterNode node;
    BoundingSphere bounds;
    std::vector<uint32_t> child_indices;  // Original indices of children
};

// ============================================================================
// Main Hierarchy Construction
// ============================================================================

bool build_hierarchy(
    MeshletData& meshlets,
    const RawMesh& mesh,
    const HierarchyParams& params_in,
    HierarchyData& out_hierarchy
) {
    // mesh parameter reserved for future use (e.g., quadric error metric computation)
    (void)mesh;

    // Sanitize parameters: 0 meshlets-per-cluster divides by zero and a
    // branching factor below 2 never converges to a root
    HierarchyParams params = params_in;
    params.meshlets_per_cluster = std::max(params.meshlets_per_cluster, 1u);
    params.branching_factor = std::max(params.branching_factor, 2u);

    out_hierarchy.clusters.clear();
    out_hierarchy.cluster_bounds.clear();
    out_hierarchy.root_clusters.clear();
    out_hierarchy.lod_levels = 1;
    out_hierarchy.total_leaf_clusters = 0;
    out_hierarchy.total_internal_clusters = 0;

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
        out_hierarchy.root_clusters.push_back(0);
        return true;
    }

    // Calculate number of LOD levels
    uint32_t num_levels = params.target_lod_levels;
    if (num_levels == 0) {
        num_levels = calculate_lod_levels(
            meshlet_count,
            params.meshlets_per_cluster,
            params.branching_factor
        );
    }

    // ========================================================================
    // Build hierarchy in a temporary structure first
    // Then reorder to ensure contiguous children
    // ========================================================================

    std::vector<ClusterInfo> temp_clusters;

    // ========================================================================
    // LEVEL 0: Create leaf clusters from meshlets
    // ========================================================================

    // Group meshlets into leaf clusters using spatial grouping
    std::vector<uint32_t> meshlet_indices(meshlet_count);
    std::iota(meshlet_indices.begin(), meshlet_indices.end(), 0);

    std::vector<SpatialGroup> leaf_groups;
    if (params.use_spatial_grouping) {
        leaf_groups = group_spatially(
            meshlets.bounds,
            meshlet_indices,
            params.meshlets_per_cluster,
            params.branching_factor
        );
    } else {
        // Simple sequential grouping (fallback)
        uint32_t groups_needed = (meshlet_count + params.meshlets_per_cluster - 1) / params.meshlets_per_cluster;
        leaf_groups.reserve(groups_needed);

        for (uint32_t i = 0; i < meshlet_count; i += params.meshlets_per_cluster) {
            SpatialGroup group;
            uint32_t count = std::min(params.meshlets_per_cluster, meshlet_count - i);
            for (uint32_t j = 0; j < count; j++) {
                group.indices.push_back(i + j);
            }
            group.bounds = merge_bounding_spheres_indexed(meshlets.bounds, group.indices);
            leaf_groups.push_back(std::move(group));
        }
    }

    // ========================================================================
    // Reorder meshlets so each leaf group occupies a contiguous range.
    // ClusterNode only stores meshlet_start/meshlet_count, but spatial
    // grouping produces arbitrary index sets — without this permutation,
    // clusters would reference meshlets belonging to other clusters.
    // Descriptor offsets into the shared vertex/index streams are absolute,
    // so permuting the descriptor/bounds/cones arrays is safe.
    // ========================================================================
    {
        std::vector<MeshletDescriptor> new_descs;
        std::vector<BoundingSphere> new_bounds;
        std::vector<NormalCone> new_cones;
        new_descs.reserve(meshlet_count);
        new_bounds.reserve(meshlet_count);
        new_cones.reserve(meshlet_count);

        uint32_t next_start = 0;
        for (auto& group : leaf_groups) {
            for (uint32_t old_idx : group.indices) {
                new_descs.push_back(meshlets.meshlets[old_idx]);
                new_bounds.push_back(meshlets.bounds[old_idx]);
                if (old_idx < meshlets.cones.size()) {
                    new_cones.push_back(meshlets.cones[old_idx]);
                }
            }
            // Rewrite the group to its new contiguous range
            uint32_t group_size = static_cast<uint32_t>(group.indices.size());
            std::iota(group.indices.begin(), group.indices.end(), next_start);
            next_start += group_size;
        }

        meshlets.meshlets = std::move(new_descs);
        meshlets.bounds = std::move(new_bounds);
        meshlets.cones = std::move(new_cones);
    }

    // Reserve space for temp clusters
    temp_clusters.reserve(leaf_groups.size() * 2);

    // Create leaf cluster infos
    std::vector<uint32_t> current_level_indices;
    current_level_indices.reserve(leaf_groups.size());

    for (const auto& group : leaf_groups) {
        ClusterInfo info;
        info.node.parent_id = INVALID_ID;
        info.node.child_start = 0;
        info.node.child_count = 0;  // Leaf nodes have no children

        // Store meshlet range (contiguous after the reorder above)
        if (!group.indices.empty()) {
            info.node.meshlet_start = group.indices.front();
            info.node.meshlet_count = static_cast<uint32_t>(group.indices.size());
        } else {
            info.node.meshlet_start = 0;
            info.node.meshlet_count = 0;
        }

        info.node.error = compute_leaf_cluster_error(meshlets, group.indices);
        info.node.parent_error = 0.0f;
        info.node.lod_level = 0;
        info.node.padding[0] = info.node.padding[1] = info.node.padding[2] = 0;
        info.bounds = group.bounds;

        uint32_t cluster_idx = static_cast<uint32_t>(temp_clusters.size());
        temp_clusters.push_back(std::move(info));
        current_level_indices.push_back(cluster_idx);
    }

    out_hierarchy.total_leaf_clusters = static_cast<uint32_t>(leaf_groups.size());

    // ========================================================================
    // LEVEL 1+: Build parent levels bottom-up
    // ========================================================================

    uint32_t current_lod_level = 1;

    while (current_level_indices.size() > 1 && current_lod_level < num_levels) {
        std::vector<uint32_t> next_level_indices;

        // Group current level clusters spatially
        std::vector<SpatialGroup> parent_groups;

        if (params.use_spatial_grouping && current_level_indices.size() > params.branching_factor) {
            // Create bounds vector for current level
            std::vector<BoundingSphere> current_bounds;
            current_bounds.reserve(current_level_indices.size());
            for (uint32_t idx : current_level_indices) {
                current_bounds.push_back(temp_clusters[idx].bounds);
            }

            // Create local indices (0 to N-1)
            std::vector<uint32_t> local_indices(current_level_indices.size());
            std::iota(local_indices.begin(), local_indices.end(), 0);

            // Group spatially
            auto groups = group_spatially(
                current_bounds,
                local_indices,
                params.branching_factor,
                params.branching_factor
            );

            // Convert local indices back to cluster indices
            for (auto& group : groups) {
                SpatialGroup converted;
                converted.indices.reserve(group.indices.size());
                for (uint32_t local_idx : group.indices) {
                    converted.indices.push_back(current_level_indices[local_idx]);
                }
                converted.bounds = group.bounds;
                converted.error = group.error;
                parent_groups.push_back(std::move(converted));
            }
        } else {
            // Simple sequential grouping
            for (size_t i = 0; i < current_level_indices.size(); i += params.branching_factor) {
                SpatialGroup group;
                uint32_t count = std::min(
                    static_cast<size_t>(params.branching_factor),
                    current_level_indices.size() - i
                );
                for (uint32_t j = 0; j < count; j++) {
                    group.indices.push_back(current_level_indices[i + j]);
                }
                parent_groups.push_back(std::move(group));
            }
        }

        // Create parent cluster infos
        for (auto& group : parent_groups) {
            ClusterInfo info;
            info.node.parent_id = INVALID_ID;
            info.node.child_start = 0;  // Will be set during reordering
            info.node.child_count = static_cast<uint32_t>(group.indices.size());
            info.node.meshlet_start = 0;
            info.node.meshlet_count = 0;
            info.node.lod_level = static_cast<uint8_t>(current_lod_level);
            info.node.padding[0] = info.node.padding[1] = info.node.padding[2] = 0;

            // Store child indices for later reordering
            info.child_indices = group.indices;

            // Compute parent error (max child error * scale)
            float max_child_error = 0.0f;
            for (uint32_t child_idx : group.indices) {
                max_child_error = std::max(max_child_error, temp_clusters[child_idx].node.error);
            }
            info.node.error = compute_level_error(max_child_error, current_lod_level, params.error_scale_per_level);
            info.node.parent_error = 0.0f;

            // Compute merged bounds
            std::vector<BoundingSphere> child_bounds;
            child_bounds.reserve(group.indices.size());
            for (uint32_t child_idx : group.indices) {
                child_bounds.push_back(temp_clusters[child_idx].bounds);
            }
            info.bounds = merge_bounding_spheres(
                child_bounds.data(),
                static_cast<uint32_t>(child_bounds.size())
            );

            uint32_t parent_idx = static_cast<uint32_t>(temp_clusters.size());

            // Update children to point to this parent
            for (uint32_t child_idx : group.indices) {
                temp_clusters[child_idx].node.parent_id = parent_idx;
                temp_clusters[child_idx].node.parent_error = info.node.error;
            }

            temp_clusters.push_back(std::move(info));
            next_level_indices.push_back(parent_idx);
        }

        out_hierarchy.total_internal_clusters += static_cast<uint32_t>(parent_groups.size());

        // Move to next level
        current_level_indices = std::move(next_level_indices);
        current_lod_level++;
    }

    // ========================================================================
    // Reorder clusters to ensure contiguous children
    // Use BFS from roots to assign final indices
    // ========================================================================

    std::vector<uint32_t> root_indices = current_level_indices;
    std::vector<uint32_t> old_to_new(temp_clusters.size(), INVALID_ID);
    std::vector<ClusterInfo> reordered;
    reordered.reserve(temp_clusters.size());

    // BFS queue: process from roots down
    std::queue<uint32_t> bfs_queue;

    // Add roots first
    for (uint32_t root_idx : root_indices) {
        old_to_new[root_idx] = static_cast<uint32_t>(reordered.size());
        reordered.push_back(std::move(temp_clusters[root_idx]));
        bfs_queue.push(root_idx);
    }

    // Process children level by level
    while (!bfs_queue.empty()) {
        uint32_t old_idx = bfs_queue.front();
        bfs_queue.pop();

        ClusterInfo& parent_info = reordered[old_to_new[old_idx]];

        if (!parent_info.child_indices.empty()) {
            // Record where children will start in the reordered array
            parent_info.node.child_start = static_cast<uint32_t>(reordered.size());

            // Add all children contiguously
            for (uint32_t old_child_idx : parent_info.child_indices) {
                old_to_new[old_child_idx] = static_cast<uint32_t>(reordered.size());
                reordered.push_back(std::move(temp_clusters[old_child_idx]));
                bfs_queue.push(old_child_idx);
            }
        }
    }

    // ========================================================================
    // Update parent_id references to use new indices
    // ========================================================================

    for (auto& info : reordered) {
        if (info.node.parent_id != INVALID_ID) {
            info.node.parent_id = old_to_new[info.node.parent_id];
        }
    }

    // ========================================================================
    // Copy to output
    // ========================================================================

    out_hierarchy.clusters.reserve(reordered.size());
    out_hierarchy.cluster_bounds.reserve(reordered.size());

    for (auto& info : reordered) {
        out_hierarchy.clusters.push_back(info.node);
        out_hierarchy.cluster_bounds.push_back(info.bounds);
    }

    // Back-reference: stamp each meshlet with its owning leaf cluster
    for (uint32_t cluster_idx = 0; cluster_idx < out_hierarchy.clusters.size(); cluster_idx++) {
        const ClusterNode& node = out_hierarchy.clusters[cluster_idx];
        if (node.child_count != 0) continue;  // Only leaves own meshlets
        for (uint32_t m = node.meshlet_start;
             m < node.meshlet_start + node.meshlet_count && m < meshlet_count; m++) {
            meshlets.meshlets[m].cluster_id = cluster_idx;
        }
    }

    // ========================================================================
    // Set root clusters (they are at the beginning after BFS reorder)
    // ========================================================================

    for (size_t i = 0; i < root_indices.size(); i++) {
        out_hierarchy.root_clusters.push_back(static_cast<uint32_t>(i));
    }

    // Set parent_error for root clusters
    for (uint32_t root_idx : out_hierarchy.root_clusters) {
        ClusterNode& root = out_hierarchy.clusters[root_idx];
        root.parent_id = INVALID_ID;
        root.parent_error = root.error * params.error_scale_per_level;
    }

    out_hierarchy.lod_levels = current_lod_level;

    return true;
}

} // namespace vgeo
