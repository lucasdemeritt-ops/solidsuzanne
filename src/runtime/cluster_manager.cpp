// VGEO Cluster Manager
// Manage cluster visibility and LOD selection

#include "cluster_manager.h"
#include "../viewer/camera.h"

#include <cmath>
#include <algorithm>

namespace vgeo {

void ClusterManager::set_asset(const VGeoAsset* asset) {
    m_asset = asset;
    m_visible.clear();
    m_culled = 0;
}

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

void ClusterManager::update(const Camera& camera, float error_threshold) {
    m_visible.clear();
    m_culled = 0;

    if (!m_asset || m_asset->clusters.empty()) {
        return;
    }

    // Reserve space for visible clusters
    m_visible.reserve(m_asset->clusters.size());

    // Simple traversal: check each leaf cluster for visibility
    // A proper implementation would traverse the hierarchy top-down
    for (size_t i = 0; i < m_asset->clusters.size(); i++) {
        const ClusterNode& cluster = m_asset->clusters[i];

        // Skip non-leaf clusters in this simple implementation
        // (leaf clusters have meshlet_count > 0)
        if (cluster.meshlet_count == 0) {
            continue;
        }

        // Get cluster bounds (use first meshlet's bounds as approximation)
        // A proper implementation would have separate cluster bounds
        BoundingSphere cluster_bounds;
        if (cluster.meshlet_start < m_asset->meshlet_bounds.size()) {
            cluster_bounds = m_asset->meshlet_bounds[cluster.meshlet_start];

            // Expand to cover all meshlets in cluster
            for (uint32_t m = 1; m < cluster.meshlet_count; m++) {
                uint32_t idx = cluster.meshlet_start + m;
                if (idx < m_asset->meshlet_bounds.size()) {
                    const BoundingSphere& mb = m_asset->meshlet_bounds[idx];
                    // Simple expansion: use max of distances + radii
                    float dx = mb.center[0] - cluster_bounds.center[0];
                    float dy = mb.center[1] - cluster_bounds.center[1];
                    float dz = mb.center[2] - cluster_bounds.center[2];
                    float dist = std::sqrt(dx*dx + dy*dy + dz*dz) + mb.radius;
                    cluster_bounds.radius = std::max(cluster_bounds.radius, dist);
                }
            }
        } else {
            // Fallback: dummy bounds
            cluster_bounds.center[0] = 0;
            cluster_bounds.center[1] = 0;
            cluster_bounds.center[2] = 0;
            cluster_bounds.radius = 1000.0f;
        }

        // Frustum culling
        if (!is_sphere_visible(cluster_bounds, camera.frustum_planes)) {
            m_culled++;
            continue;
        }

        // Calculate screen-space error for LOD selection
        float distance = distance_to_sphere(camera.position, cluster_bounds);
        float screen_error = camera.compute_screen_error(cluster.error, distance);

        // LOD cut: skip if error is below threshold (would use parent instead)
        // For now, accept all visible leaf clusters since we only have 2 levels
        // A proper DAG cut would check: child.error > threshold && parent.error <= threshold

        // Check parent error for proper LOD cut
        if (cluster.parent_error > 0 && cluster.parent_id != INVALID_ID) {
            float parent_screen_error = camera.compute_screen_error(cluster.parent_error, distance);

            // If parent is acceptable, we could use coarser LOD
            // But since we don't have simplified parent geometry yet, always use leaves
            (void)parent_screen_error;  // Suppress unused warning
        }

        // Add to visible list
        VisibleCluster vc;
        vc.cluster_id = static_cast<uint32_t>(i);
        vc.meshlet_start = cluster.meshlet_start;
        vc.meshlet_count = cluster.meshlet_count;
        vc.screen_error = screen_error;

        m_visible.push_back(vc);
    }

    // Sort by screen error (draw high-detail first for early-z)
    std::sort(m_visible.begin(), m_visible.end(),
        [](const VisibleCluster& a, const VisibleCluster& b) {
            return a.screen_error > b.screen_error;
        });
}

const std::vector<VisibleCluster>& ClusterManager::visible_clusters() const {
    return m_visible;
}

uint32_t ClusterManager::total_meshlets_visible() const {
    uint32_t total = 0;
    for (const auto& vc : m_visible) {
        total += vc.meshlet_count;
    }
    return total;
}

uint32_t ClusterManager::clusters_culled() const {
    return m_culled;
}

} // namespace vgeo
