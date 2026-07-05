#pragma once

#include "vgeo_loader.h"
#include <vector>
#include <cstdint>

namespace vgeo {

struct Camera;  // Forward declare

// Visible cluster for this frame
struct VisibleCluster {
    uint32_t cluster_id;
    uint32_t meshlet_start;
    uint32_t meshlet_count;
    float screen_error;
    uint8_t lod_level;  // LOD level of this cluster (0 = finest)
};

// LOD selection parameters
struct LODParams {
    float error_threshold = 1.0f;        // Screen-space error threshold in pixels
    float error_bias = 0.0f;             // Bias to add/subtract from error (debugging)
    bool force_max_lod = false;          // Always use finest LOD (debug)
    bool force_min_lod = false;          // Always use coarsest LOD (debug)
    uint32_t max_visible_clusters = 0;   // 0 = unlimited
};

// Statistics for the current frame
struct LODStats {
    uint32_t total_clusters_visited;     // Clusters examined during traversal
    uint32_t clusters_rendered;          // Clusters selected for rendering
    uint32_t clusters_culled_frustum;    // Culled by frustum
    uint32_t clusters_culled_lod;        // Skipped due to LOD (parent selected instead)
    uint32_t meshlets_rendered;          // Total meshlets to render
    uint32_t lod_levels_used;            // Number of different LOD levels in cut
    float min_screen_error;              // Minimum screen error in visible set
    float max_screen_error;              // Maximum screen error in visible set
};

class ClusterManager {
public:
    // Set the loaded asset
    void set_asset(const VGeoAsset* asset);

    // Set cluster bounds (from hierarchy construction or loaded from file)
    void set_cluster_bounds(const std::vector<BoundingSphere>& bounds);

    // Set root cluster indices for multi-root hierarchies
    void set_root_clusters(const std::vector<uint32_t>& roots);

    // Update visibility for camera using DAG traversal
    void update(const Camera& camera, const LODParams& params);

    // Simplified update with default parameters
    void update(const Camera& camera, float error_threshold);

    // Get visible clusters for rendering
    const std::vector<VisibleCluster>& visible_clusters() const;

    // Get statistics
    const LODStats& stats() const { return m_stats; }

    // Legacy stats accessors
    uint32_t total_meshlets_visible() const;
    uint32_t clusters_culled() const;

private:
    // Recursive DAG traversal for LOD cut selection
    void traverse_cluster(
        uint32_t cluster_id,
        const Camera& camera,
        const LODParams& params,
        const float frustum_planes[6][4]
    );

    // Check if cluster should be rendered (is part of LOD cut)
    bool should_render_cluster(
        const ClusterNode& cluster,
        const BoundingSphere& bounds,
        const Camera& camera,
        const LODParams& params,
        float& out_screen_error
    );

    // Collect all leaf meshlets from a cluster subtree (for coarse LOD fallback)
    void collect_leaf_meshlets(
        uint32_t cluster_id,
        std::vector<uint32_t>& meshlet_starts,
        std::vector<uint32_t>& meshlet_counts,
        uint32_t depth = 0
    );

    const VGeoAsset* m_asset = nullptr;
    std::vector<BoundingSphere> m_cluster_bounds;
    std::vector<uint32_t> m_root_clusters;
    std::vector<VisibleCluster> m_visible;
    // Per-update visited set: prevents duplicate draws for shared DAG
    // children and unbounded recursion on malformed files with cycles
    std::vector<bool> m_visited;
    LODStats m_stats;
};

} // namespace vgeo
