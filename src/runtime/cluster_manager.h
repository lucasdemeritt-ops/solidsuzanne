#pragma once

#include "vgeo_loader.h"
#include <vector>

namespace vgeo {

struct Camera;  // Forward declare

// Visible cluster for this frame
struct VisibleCluster {
    uint32_t cluster_id;
    uint32_t meshlet_start;
    uint32_t meshlet_count;
    float screen_error;
};

class ClusterManager {
public:
    // Set the loaded asset
    void set_asset(const VGeoAsset* asset);

    // Update visibility for camera
    void update(const Camera& camera, float error_threshold);

    // Get visible clusters for rendering
    const std::vector<VisibleCluster>& visible_clusters() const;

    // Stats
    uint32_t total_meshlets_visible() const;
    uint32_t clusters_culled() const;

private:
    const VGeoAsset* m_asset = nullptr;
    std::vector<VisibleCluster> m_visible;
    uint32_t m_culled = 0;
};

} // namespace vgeo
