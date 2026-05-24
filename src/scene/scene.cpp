// VGEO Scene Graph Implementation

#include "scene.h"
#include "camera.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <limits>

namespace vgeo {

// ============================================================================
// Matrix Utilities
// ============================================================================

namespace matrix {

void identity(float* out) {
    std::memset(out, 0, 16 * sizeof(float));
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

void translation(float* out, float x, float y, float z) {
    identity(out);
    out[12] = x;
    out[13] = y;
    out[14] = z;
}

void rotation(float* out, float rx, float ry, float rz) {
    // Rotation order: Z * Y * X
    float cx = std::cos(rx), sx = std::sin(rx);
    float cy = std::cos(ry), sy = std::sin(ry);
    float cz = std::cos(rz), sz = std::sin(rz);

    out[0] = cy * cz;
    out[1] = cy * sz;
    out[2] = -sy;
    out[3] = 0;

    out[4] = sx * sy * cz - cx * sz;
    out[5] = sx * sy * sz + cx * cz;
    out[6] = sx * cy;
    out[7] = 0;

    out[8] = cx * sy * cz + sx * sz;
    out[9] = cx * sy * sz - sx * cz;
    out[10] = cx * cy;
    out[11] = 0;

    out[12] = 0;
    out[13] = 0;
    out[14] = 0;
    out[15] = 1;
}

void scale(float* out, float sx, float sy, float sz) {
    identity(out);
    out[0] = sx;
    out[5] = sy;
    out[10] = sz;
}

void multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            tmp[col * 4 + row] = 0.0f;
            for (int k = 0; k < 4; k++) {
                tmp[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
            }
        }
    }
    std::memcpy(out, tmp, sizeof(tmp));
}

void transform_point(float* out, const float* m, const float* p) {
    float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    out[0] = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / w;
    out[1] = (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / w;
    out[2] = (m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]) / w;
}

void transform_aabb(float* out_min, float* out_max,
                    const float* m,
                    const float* in_min, const float* in_max) {
    // Transform all 8 corners and find new AABB
    float corners[8][3] = {
        {in_min[0], in_min[1], in_min[2]},
        {in_max[0], in_min[1], in_min[2]},
        {in_min[0], in_max[1], in_min[2]},
        {in_max[0], in_max[1], in_min[2]},
        {in_min[0], in_min[1], in_max[2]},
        {in_max[0], in_min[1], in_max[2]},
        {in_min[0], in_max[1], in_max[2]},
        {in_max[0], in_max[1], in_max[2]},
    };

    out_min[0] = out_min[1] = out_min[2] = std::numeric_limits<float>::max();
    out_max[0] = out_max[1] = out_max[2] = std::numeric_limits<float>::lowest();

    for (int i = 0; i < 8; i++) {
        float transformed[3];
        transform_point(transformed, m, corners[i]);

        out_min[0] = std::min(out_min[0], transformed[0]);
        out_min[1] = std::min(out_min[1], transformed[1]);
        out_min[2] = std::min(out_min[2], transformed[2]);
        out_max[0] = std::max(out_max[0], transformed[0]);
        out_max[1] = std::max(out_max[1], transformed[1]);
        out_max[2] = std::max(out_max[2], transformed[2]);
    }
}

} // namespace matrix

// ============================================================================
// Scene Implementation
// ============================================================================

AssetId Scene::load_asset(const std::string& path) {
    // Check cache first
    auto it = m_asset_path_cache.find(path);
    if (it != m_asset_path_cache.end()) {
        return it->second;
    }

    // Load the asset
    auto asset = load_vgeo(path);
    if (!asset) {
        return INVALID_ASSET_ID;
    }

    // Assign ID and store
    AssetId id = m_next_asset_id++;
    m_assets[id] = std::move(asset);
    m_asset_path_cache[path] = id;
    m_asset_refs[id] = 0;

    return id;
}

bool Scene::unload_asset(AssetId asset_id) {
    auto it = m_asset_refs.find(asset_id);
    if (it == m_asset_refs.end()) {
        return false;
    }

    // Only unload if no references
    if (it->second > 0) {
        return false;
    }

    // Remove from caches
    m_assets.erase(asset_id);
    m_asset_refs.erase(asset_id);

    // Remove from path cache
    for (auto pit = m_asset_path_cache.begin(); pit != m_asset_path_cache.end(); ++pit) {
        if (pit->second == asset_id) {
            m_asset_path_cache.erase(pit);
            break;
        }
    }

    return true;
}

const VGeoAsset* Scene::get_asset(AssetId asset_id) const {
    auto it = m_assets.find(asset_id);
    return (it != m_assets.end()) ? it->second.get() : nullptr;
}

ObjectId Scene::add_object(AssetId asset_id, const float* transform, const char* name) {
    if (m_assets.find(asset_id) == m_assets.end()) {
        return INVALID_OBJECT_ID;
    }

    SceneObject obj;
    obj.id = m_next_object_id++;
    obj.asset_id = asset_id;

    if (transform) {
        std::memcpy(obj.transform, transform, 16 * sizeof(float));
    } else {
        matrix::identity(obj.transform);
    }

    if (name) {
        obj.name = name;
    }

    obj.bounds_dirty = true;

    // Increment asset reference count
    m_asset_refs[asset_id]++;

    m_objects.push_back(std::move(obj));

    // Update bounds immediately
    update_object_bounds(m_objects.back());

    return m_objects.back().id;
}

ObjectId Scene::add_object_from_file(const std::string& path, const float* transform, const char* name) {
    AssetId asset_id = load_asset(path);
    if (asset_id == INVALID_ASSET_ID) {
        return INVALID_OBJECT_ID;
    }
    return add_object(asset_id, transform, name);
}

bool Scene::remove_object(ObjectId object_id) {
    int idx = find_object_index(object_id);
    if (idx < 0) {
        return false;
    }

    // Decrement asset reference count
    AssetId asset_id = m_objects[idx].asset_id;
    if (m_asset_refs.count(asset_id) > 0) {
        m_asset_refs[asset_id]--;
    }

    // Remove object
    m_objects.erase(m_objects.begin() + idx);

    return true;
}

SceneObject* Scene::get_object(ObjectId object_id) {
    int idx = find_object_index(object_id);
    return (idx >= 0) ? &m_objects[idx] : nullptr;
}

const SceneObject* Scene::get_object(ObjectId object_id) const {
    int idx = find_object_index(object_id);
    return (idx >= 0) ? &m_objects[idx] : nullptr;
}

void Scene::set_transform(ObjectId object_id, const float* transform) {
    SceneObject* obj = get_object(object_id);
    if (obj) {
        std::memcpy(obj->transform, transform, 16 * sizeof(float));
        obj->bounds_dirty = true;
    }
}

void Scene::set_position(ObjectId object_id, float x, float y, float z) {
    SceneObject* obj = get_object(object_id);
    if (obj) {
        obj->transform[12] = x;
        obj->transform[13] = y;
        obj->transform[14] = z;
        obj->bounds_dirty = true;
    }
}

void Scene::set_rotation(ObjectId object_id, float rx, float ry, float rz) {
    SceneObject* obj = get_object(object_id);
    if (!obj) return;

    // Preserve translation and scale, replace rotation
    float tx = obj->transform[12];
    float ty = obj->transform[13];
    float tz = obj->transform[14];

    // Extract current scale (approximate - assumes no shear)
    float sx = std::sqrt(obj->transform[0]*obj->transform[0] +
                         obj->transform[1]*obj->transform[1] +
                         obj->transform[2]*obj->transform[2]);
    float sy = std::sqrt(obj->transform[4]*obj->transform[4] +
                         obj->transform[5]*obj->transform[5] +
                         obj->transform[6]*obj->transform[6]);
    float sz = std::sqrt(obj->transform[8]*obj->transform[8] +
                         obj->transform[9]*obj->transform[9] +
                         obj->transform[10]*obj->transform[10]);

    // Build new transform: T * R * S
    float rot[16], scl[16], tmp[16];
    matrix::rotation(rot, rx, ry, rz);
    matrix::scale(scl, sx, sy, sz);
    matrix::multiply(tmp, rot, scl);

    std::memcpy(obj->transform, tmp, 16 * sizeof(float));
    obj->transform[12] = tx;
    obj->transform[13] = ty;
    obj->transform[14] = tz;
    obj->bounds_dirty = true;
}

void Scene::set_scale(ObjectId object_id, float scale) {
    set_scale(object_id, scale, scale, scale);
}

void Scene::set_scale(ObjectId object_id, float sx, float sy, float sz) {
    SceneObject* obj = get_object(object_id);
    if (!obj) return;

    // Normalize current matrix columns and apply new scale
    float len0 = std::sqrt(obj->transform[0]*obj->transform[0] +
                           obj->transform[1]*obj->transform[1] +
                           obj->transform[2]*obj->transform[2]);
    float len1 = std::sqrt(obj->transform[4]*obj->transform[4] +
                           obj->transform[5]*obj->transform[5] +
                           obj->transform[6]*obj->transform[6]);
    float len2 = std::sqrt(obj->transform[8]*obj->transform[8] +
                           obj->transform[9]*obj->transform[9] +
                           obj->transform[10]*obj->transform[10]);

    if (len0 > 1e-6f) {
        obj->transform[0] = (obj->transform[0] / len0) * sx;
        obj->transform[1] = (obj->transform[1] / len0) * sx;
        obj->transform[2] = (obj->transform[2] / len0) * sx;
    }
    if (len1 > 1e-6f) {
        obj->transform[4] = (obj->transform[4] / len1) * sy;
        obj->transform[5] = (obj->transform[5] / len1) * sy;
        obj->transform[6] = (obj->transform[6] / len1) * sy;
    }
    if (len2 > 1e-6f) {
        obj->transform[8] = (obj->transform[8] / len2) * sz;
        obj->transform[9] = (obj->transform[9] / len2) * sz;
        obj->transform[10] = (obj->transform[10] / len2) * sz;
    }

    obj->bounds_dirty = true;
}

void Scene::set_visible(ObjectId object_id, bool visible) {
    SceneObject* obj = get_object(object_id);
    if (obj) {
        obj->visible = visible;
    }
}

void Scene::set_selected(ObjectId object_id, bool selected) {
    SceneObject* obj = get_object(object_id);
    if (obj) {
        obj->selected = selected;
    }
}

void Scene::update() {
    for (auto& obj : m_objects) {
        if (obj.bounds_dirty) {
            update_object_bounds(obj);
        }
    }
}

void Scene::update_object_bounds(SceneObject& obj) {
    const VGeoAsset* asset = get_asset(obj.asset_id);
    if (!asset) {
        obj.bounds_min[0] = obj.bounds_min[1] = obj.bounds_min[2] = 0;
        obj.bounds_max[0] = obj.bounds_max[1] = obj.bounds_max[2] = 0;
        obj.bounds_dirty = false;
        return;
    }

    const AABB& local_bounds = asset->header.bounds;
    matrix::transform_aabb(
        obj.bounds_min, obj.bounds_max,
        obj.transform,
        local_bounds.min, local_bounds.max
    );

    obj.bounds_dirty = false;
}

// Frustum-AABB intersection test
static bool aabb_in_frustum(const float frustum_planes[6][4],
                            const float* box_min, const float* box_max) {
    for (int i = 0; i < 6; i++) {
        const float* plane = frustum_planes[i];

        // Find the corner most in the direction of the plane normal
        float px = (plane[0] > 0) ? box_max[0] : box_min[0];
        float py = (plane[1] > 0) ? box_max[1] : box_min[1];
        float pz = (plane[2] > 0) ? box_max[2] : box_min[2];

        float dist = plane[0] * px + plane[1] * py + plane[2] * pz + plane[3];

        if (dist < 0) {
            return false;  // Entirely outside this plane
        }
    }
    return true;
}

void Scene::collect_draw_commands(
    const Camera& camera,
    float error_threshold,
    std::vector<DrawCommand>& out_commands
) {
    // TODO: error_threshold will gate LOD-cut selection once internal clusters
    // carry simplified geometry; for now all leaf clusters are emitted.
    (void)error_threshold;

    out_commands.clear();

    for (const auto& obj : m_objects) {
        if (!obj.visible) continue;

        // Frustum cull the object
        if (!aabb_in_frustum(camera.frustum_planes, obj.bounds_min, obj.bounds_max)) {
            continue;
        }

        const VGeoAsset* asset = get_asset(obj.asset_id);
        if (!asset) continue;

        // For each cluster in the asset, perform LOD selection
        // (Simplified: for now, just add all leaf clusters)
        for (const auto& cluster : asset->clusters) {
            if (cluster.meshlet_count == 0) continue;  // Skip internal nodes

            DrawCommand cmd;
            cmd.object_id = obj.id;
            cmd.cluster_id = &cluster - asset->clusters.data();
            cmd.meshlet_start = cluster.meshlet_start;
            cmd.meshlet_count = cluster.meshlet_count;
            cmd.lod_level = cluster.lod_level;
            cmd.transform = obj.transform;

            // Compute screen error (approximate using object center distance)
            float center[3] = {
                (obj.bounds_min[0] + obj.bounds_max[0]) * 0.5f,
                (obj.bounds_min[1] + obj.bounds_max[1]) * 0.5f,
                (obj.bounds_min[2] + obj.bounds_max[2]) * 0.5f
            };
            float dx = center[0] - camera.position[0];
            float dy = center[1] - camera.position[1];
            float dz = center[2] - camera.position[2];
            float distance = std::sqrt(dx*dx + dy*dy + dz*dz);

            cmd.screen_error = camera.compute_screen_error(cluster.error, distance);

            out_commands.push_back(cmd);
        }
    }

    // Sort by screen error (front to back for early-z)
    std::sort(out_commands.begin(), out_commands.end(),
        [](const DrawCommand& a, const DrawCommand& b) {
            return a.screen_error > b.screen_error;
        });
}

SceneStats Scene::get_stats() const {
    SceneStats stats;
    stats.total_objects = static_cast<uint32_t>(m_objects.size());
    stats.unique_assets = static_cast<uint32_t>(m_assets.size());

    for (const auto& obj : m_objects) {
        if (obj.visible) {
            stats.visible_objects++;
        }

        const VGeoAsset* asset = get_asset(obj.asset_id);
        if (asset) {
            stats.total_clusters += static_cast<uint32_t>(asset->clusters.size());
            stats.total_meshlets += asset->header.meshlet_count;
        }
    }

    // Count instanced objects (objects sharing an asset with another)
    for (const auto& ref : m_asset_refs) {
        if (ref.second > 1) {
            stats.instanced_objects += ref.second;
        }
    }

    return stats;
}

ObjectId Scene::pick_object(const float* ray_origin, const float* ray_direction) const {
    float closest_t = std::numeric_limits<float>::max();
    ObjectId closest_id = INVALID_OBJECT_ID;

    for (const auto& obj : m_objects) {
        if (!obj.visible) continue;

        // Ray-AABB intersection
        float tmin = 0.0f;
        float tmax = std::numeric_limits<float>::max();

        for (int i = 0; i < 3; i++) {
            if (std::abs(ray_direction[i]) < 1e-8f) {
                // Ray parallel to slab
                if (ray_origin[i] < obj.bounds_min[i] || ray_origin[i] > obj.bounds_max[i]) {
                    tmin = std::numeric_limits<float>::max();
                    break;
                }
            } else {
                float t1 = (obj.bounds_min[i] - ray_origin[i]) / ray_direction[i];
                float t2 = (obj.bounds_max[i] - ray_origin[i]) / ray_direction[i];

                if (t1 > t2) std::swap(t1, t2);

                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);

                if (tmin > tmax) {
                    tmin = std::numeric_limits<float>::max();
                    break;
                }
            }
        }

        if (tmin < closest_t && tmin >= 0) {
            closest_t = tmin;
            closest_id = obj.id;
        }
    }

    return closest_id;
}

void Scene::get_bounds(float* out_min, float* out_max) const {
    out_min[0] = out_min[1] = out_min[2] = std::numeric_limits<float>::max();
    out_max[0] = out_max[1] = out_max[2] = std::numeric_limits<float>::lowest();

    for (const auto& obj : m_objects) {
        out_min[0] = std::min(out_min[0], obj.bounds_min[0]);
        out_min[1] = std::min(out_min[1], obj.bounds_min[1]);
        out_min[2] = std::min(out_min[2], obj.bounds_min[2]);
        out_max[0] = std::max(out_max[0], obj.bounds_max[0]);
        out_max[1] = std::max(out_max[1], obj.bounds_max[1]);
        out_max[2] = std::max(out_max[2], obj.bounds_max[2]);
    }

    // Handle empty scene
    if (m_objects.empty()) {
        out_min[0] = out_min[1] = out_min[2] = 0;
        out_max[0] = out_max[1] = out_max[2] = 0;
    }
}

void Scene::clear() {
    m_objects.clear();
    m_assets.clear();
    m_asset_refs.clear();
    m_asset_path_cache.clear();
}

int Scene::find_object_index(ObjectId object_id) const {
    for (size_t i = 0; i < m_objects.size(); i++) {
        if (m_objects[i].id == object_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace vgeo
