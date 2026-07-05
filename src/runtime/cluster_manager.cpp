// VGEO Cluster Manager
// Manage cluster visibility and LOD selection using DAG traversal

#include "cluster_manager.h"
#include "camera.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <set>

namespace vgeo {

// ============================================================================
// Frustum Culling
// ============================================================================

// Test if a bounding sphere is visible in the frustum
static bool is_sphere_visible(
    const BoundingSphere& sphere,
    const float frustum_planes[6][4]
) {
    for (int i = 0; i < 6; i++) {
        const float* plane = frustum_planes[i];
        float dist = plane[0] * sphere.center[0] +
                     plane[1] * sphere.center[1] +
                     plane[2] * sphere.center[2] +
                     plane[3];

        // Sphere is completely outside this plane
        if (dist < -sphere.radius) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Distance Calculations
// ============================================================================

// Compute distance from camera to sphere center
static float distance_to_sphere(
    const float camera_pos[3],
    const BoundingSphere& sphere
) {
    float dx = sphere.center[0] - camera_pos[0];
    float dy = sphere.center[1] - camera_pos[1];
    float dz = sphere.center[2] - camera_pos[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ============================================================================
// ClusterManager Implementation
// ============================================================================

void ClusterManager::set_asset(const VGeoAsset* asset) {
    m_asset = asset;
    m_visible.clear();
    m_stats = {};
    m_root_clusters.clear();
    m_cluster_bounds.clear();

    if (!asset || asset->clusters.empty()) {
        return;
    }

    // Load cluster bounds if available in asset
    if (!asset->cluster_bounds.empty()) {
        m_cluster_bounds = asset->cluster_bounds;
    }

    // Auto-detect root clusters
    // Find clusters with no parent (roots)
    for (size_t i = 0; i < asset->clusters.size(); i++) {
        if (asset->clusters[i].parent_id == INVALID_ID) {
            m_root_clusters.push_back(static_cast<uint32_t>(i));
        }
    }

    // Fallback: if no explicit roots found, find highest LOD level clusters
    if (m_root_clusters.empty()) {
        uint8_t max_lod = 0;
        for (const auto& cluster : asset->clusters) {
            max_lod = std::max(max_lod, cluster.lod_level);
        }
        for (size_t i = 0; i < asset->clusters.size(); i++) {
            if (asset->clusters[i].lod_level == max_lod) {
                m_root_clusters.push_back(static_cast<uint32_t>(i));
            }
        }
    }
}

void ClusterManager::set_cluster_bounds(const std::vector<BoundingSphere>& bounds) {
    m_cluster_bounds = bounds;
}

void ClusterManager::set_root_clusters(const std::vector<uint32_t>& roots) {
    m_root_clusters = roots;
}

void ClusterManager::update(const Camera& camera, float error_threshold) {
    LODParams params;
    params.error_threshold = error_threshold;
    update(camera, params);
}

void ClusterManager::update(const Camera& camera, const LODParams& params) {
    m_visible.clear();
    m_stats = {};
    m_stats.min_screen_error = std::numeric_limits<float>::max();
    m_stats.max_screen_error = 0.0f;

    if (!m_asset || m_asset->clusters.empty()) {
        return;
    }

    // Reserve space for visible clusters
    m_visible.reserve(m_asset->clusters.size());

    // If no roots defined, fall back to simple leaf traversal
    if (m_root_clusters.empty()) {
        // Legacy mode: render all leaf clusters
        for (size_t i = 0; i < m_asset->clusters.size(); i++) {
            const ClusterNode& cluster = m_asset->clusters[i];
            if (cluster.meshlet_count == 0) continue;  // Skip non-leaf

            m_stats.total_clusters_visited++;

            // Get bounds
            BoundingSphere bounds;
            if (i < m_cluster_bounds.size()) {
                bounds = m_cluster_bounds[i];
            } else if (cluster.meshlet_start < m_asset->meshlet_bounds.size()) {
                bounds = m_asset->meshlet_bounds[cluster.meshlet_start];
                bounds.radius *= 2.0f;  // Rough approximation
            } else {
                bounds = {{0, 0, 0}, 1000.0f};
            }

            // Frustum cull
            if (!is_sphere_visible(bounds, camera.frustum_planes)) {
                m_stats.clusters_culled_frustum++;
                continue;
            }

            // Compute screen error
            float distance = distance_to_sphere(camera.position, bounds);
            float screen_error = camera.compute_screen_error(cluster.error, distance);

            VisibleCluster vc;
            vc.cluster_id = static_cast<uint32_t>(i);
            vc.meshlet_start = cluster.meshlet_start;
            vc.meshlet_count = cluster.meshlet_count;
            vc.screen_error = screen_error;
            vc.lod_level = cluster.lod_level;

            m_visible.push_back(vc);
            m_stats.clusters_rendered++;
            m_stats.meshlets_rendered += cluster.meshlet_count;
        }
    } else {
        // DAG traversal from roots
        m_visited.assign(m_asset->clusters.size(), false);
        for (uint32_t root_id : m_root_clusters) {
            traverse_cluster(root_id, camera, params, camera.frustum_planes);
        }
    }

    // Update statistics
    std::set<uint8_t> lod_levels;
    for (const auto& vc : m_visible) {
        lod_levels.insert(vc.lod_level);
        m_stats.min_screen_error = std::min(m_stats.min_screen_error, vc.screen_error);
        m_stats.max_screen_error = std::max(m_stats.max_screen_error, vc.screen_error);
    }
    m_stats.lod_levels_used = static_cast<uint32_t>(lod_levels.size());

    if (m_visible.empty()) {
        m_stats.min_screen_error = 0.0f;
    }

    // Sort by screen error (high detail / high error first for early-z benefit)
    std::sort(m_visible.begin(), m_visible.end(),
        [](const VisibleCluster& a, const VisibleCluster& b) {
            return a.screen_error > b.screen_error;
        });

    // Apply max visible limit if set
    if (params.max_visible_clusters > 0 && m_visible.size() > params.max_visible_clusters) {
        m_visible.resize(params.max_visible_clusters);
        // Recalculate meshlet count
        m_stats.meshlets_rendered = 0;
        for (const auto& vc : m_visible) {
            m_stats.meshlets_rendered += vc.meshlet_count;
        }
    }
}

void ClusterManager::traverse_cluster(
    uint32_t cluster_id,
    const Camera& camera,
    const LODParams& params,
    const float frustum_planes[6][4]
) {
    if (cluster_id >= m_asset->clusters.size()) {
        return;
    }

    // Visit each cluster at most once per update: DAG children can be
    // shared between parents (double draws), and a malformed file with a
    // child pointing at an ancestor would recurse forever
    if (cluster_id < m_visited.size()) {
        if (m_visited[cluster_id]) {
            return;
        }
        m_visited[cluster_id] = true;
    }

    m_stats.total_clusters_visited++;

    const ClusterNode& cluster = m_asset->clusters[cluster_id];

    // Get bounds for this cluster
    BoundingSphere bounds;
    if (cluster_id < m_cluster_bounds.size()) {
        bounds = m_cluster_bounds[cluster_id];
    } else if (cluster.meshlet_count > 0 && cluster.meshlet_start < m_asset->meshlet_bounds.size()) {
        // Leaf cluster: compute bounds from meshlets
        bounds = m_asset->meshlet_bounds[cluster.meshlet_start];
        for (uint32_t m = 1; m < cluster.meshlet_count; m++) {
            uint32_t idx = cluster.meshlet_start + m;
            if (idx < m_asset->meshlet_bounds.size()) {
                const BoundingSphere& mb = m_asset->meshlet_bounds[idx];
                float dx = mb.center[0] - bounds.center[0];
                float dy = mb.center[1] - bounds.center[1];
                float dz = mb.center[2] - bounds.center[2];
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz) + mb.radius;
                bounds.radius = std::max(bounds.radius, dist);
            }
        }
    } else if (cluster.child_count > 0) {
        // Internal node: compute bounds from children
        // This is a fallback; ideally bounds are precomputed
        bounds = {{0, 0, 0}, 1000.0f};  // Conservative fallback
    } else {
        bounds = {{0, 0, 0}, 1000.0f};
    }

    // Frustum culling - if cluster is outside frustum, skip entire subtree
    if (!is_sphere_visible(bounds, frustum_planes)) {
        m_stats.clusters_culled_frustum++;
        return;
    }

    // Calculate screen-space error for this cluster
    float distance = distance_to_sphere(camera.position, bounds);
    float screen_error = camera.compute_screen_error(cluster.error, distance);

    // Apply error bias
    screen_error += params.error_bias;

    // Debug modes
    if (params.force_max_lod) {
        // Force finest LOD: always recurse to leaves
        if (cluster.child_count > 0) {
            // Has children, recurse
            for (uint32_t i = 0; i < cluster.child_count; i++) {
                traverse_cluster(cluster.child_start + i, camera, params, frustum_planes);
            }
            return;
        }
        // Is leaf, fall through to render
    } else if (params.force_min_lod) {
        // Force coarsest LOD: render this cluster (if it has geometry)
        // For internal nodes without geometry, we need to collect leaf meshlets
        if (cluster.meshlet_count > 0) {
            // Has geometry, render it
            VisibleCluster vc;
            vc.cluster_id = cluster_id;
            vc.meshlet_start = cluster.meshlet_start;
            vc.meshlet_count = cluster.meshlet_count;
            vc.screen_error = screen_error;
            vc.lod_level = cluster.lod_level;
            m_visible.push_back(vc);
            m_stats.clusters_rendered++;
            m_stats.meshlets_rendered += cluster.meshlet_count;
        } else {
            // Internal node without geometry - collect all descendant leaves
            std::vector<uint32_t> meshlet_starts, meshlet_counts;
            collect_leaf_meshlets(cluster_id, meshlet_starts, meshlet_counts);
            // Note: In a full implementation, we'd batch these or have simplified proxy geometry
            // For now, this is a fallback that renders all leaves
            for (size_t i = 0; i < meshlet_starts.size(); i++) {
                VisibleCluster vc;
                vc.cluster_id = cluster_id;  // Note: Using parent cluster ID
                vc.meshlet_start = meshlet_starts[i];
                vc.meshlet_count = meshlet_counts[i];
                vc.screen_error = screen_error;
                vc.lod_level = cluster.lod_level;
                m_visible.push_back(vc);
                m_stats.meshlets_rendered += meshlet_counts[i];
            }
            m_stats.clusters_rendered++;
        }
        return;
    }

    // Normal LOD selection logic:
    // Compare screen error to threshold to decide whether to:
    // 1. Render this cluster (error acceptable), or
    // 2. Recurse into children (error too high, need more detail)

    bool is_leaf = (cluster.child_count == 0);
    bool has_geometry = (cluster.meshlet_count > 0);

    // LOD cut decision
    // Render this cluster if:
    // - Screen error is acceptable (below threshold), OR
    // - This is a leaf node (no finer detail available)
    bool should_render = (screen_error <= params.error_threshold) || is_leaf;

    if (should_render) {
        if (has_geometry) {
            // This cluster has renderable geometry
            VisibleCluster vc;
            vc.cluster_id = cluster_id;
            vc.meshlet_start = cluster.meshlet_start;
            vc.meshlet_count = cluster.meshlet_count;
            vc.screen_error = screen_error;
            vc.lod_level = cluster.lod_level;
            m_visible.push_back(vc);
            m_stats.clusters_rendered++;
            m_stats.meshlets_rendered += cluster.meshlet_count;
        } else if (is_leaf) {
            // Leaf without geometry - unusual, skip
            m_stats.clusters_culled_lod++;
        } else {
            // Internal node without geometry but error is acceptable
            // This can happen when error is acceptable but we don't have
            // simplified proxy geometry for this level.
            // Fall back to rendering all descendant leaves.
            for (uint32_t i = 0; i < cluster.child_count; i++) {
                traverse_cluster(cluster.child_start + i, camera, params, frustum_planes);
            }
        }
    } else {
        // Error too high - need more detail, recurse into children
        if (cluster.child_count > 0) {
            m_stats.clusters_culled_lod++;  // This cluster skipped in favor of children
            for (uint32_t i = 0; i < cluster.child_count; i++) {
                traverse_cluster(cluster.child_start + i, camera, params, frustum_planes);
            }
        } else {
            // No children available but error is high
            // This shouldn't happen in a well-formed hierarchy
            if (has_geometry) {
                VisibleCluster vc;
                vc.cluster_id = cluster_id;
                vc.meshlet_start = cluster.meshlet_start;
                vc.meshlet_count = cluster.meshlet_count;
                vc.screen_error = screen_error;
                vc.lod_level = cluster.lod_level;
                m_visible.push_back(vc);
                m_stats.clusters_rendered++;
                m_stats.meshlets_rendered += cluster.meshlet_count;
            }
        }
    }
}

bool ClusterManager::should_render_cluster(
    const ClusterNode& cluster,
    const BoundingSphere& bounds,
    const Camera& camera,
    const LODParams& params,
    float& out_screen_error
) {
    float distance = distance_to_sphere(camera.position, bounds);
    out_screen_error = camera.compute_screen_error(cluster.error, distance);
    out_screen_error += params.error_bias;

    // Render if error is acceptable or if this is a leaf
    bool is_leaf = (cluster.child_count == 0);
    return (out_screen_error <= params.error_threshold) || is_leaf;
}

void ClusterManager::collect_leaf_meshlets(
    uint32_t cluster_id,
    std::vector<uint32_t>& meshlet_starts,
    std::vector<uint32_t>& meshlet_counts,
    uint32_t depth
) {
    if (cluster_id >= m_asset->clusters.size()) {
        return;
    }

    // Hierarchies are capped at 16 LOD levels; anything deeper means a
    // malformed file with a cycle
    if (depth > 64) {
        return;
    }

    const ClusterNode& cluster = m_asset->clusters[cluster_id];

    if (cluster.meshlet_count > 0) {
        // This is a leaf (or has direct meshlets)
        meshlet_starts.push_back(cluster.meshlet_start);
        meshlet_counts.push_back(cluster.meshlet_count);
    }

    // Recurse into children
    for (uint32_t i = 0; i < cluster.child_count; i++) {
        collect_leaf_meshlets(cluster.child_start + i, meshlet_starts, meshlet_counts, depth + 1);
    }
}

const std::vector<VisibleCluster>& ClusterManager::visible_clusters() const {
    return m_visible;
}

uint32_t ClusterManager::total_meshlets_visible() const {
    return m_stats.meshlets_rendered;
}

uint32_t ClusterManager::clusters_culled() const {
    return m_stats.clusters_culled_frustum + m_stats.clusters_culled_lod;
}

} // namespace vgeo
