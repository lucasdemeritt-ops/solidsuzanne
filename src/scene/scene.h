#pragma once

// VGEO Scene Graph
// Manages multiple virtualized geometry objects with transforms and instancing

#include "vgeo_format.h"
#include "vgeo_loader.h"

#include <vector>
#include <unordered_map>
#include <memory>
#include <string>

namespace vgeo {

// Forward declarations
struct Camera;

// Unique identifier for objects in the scene
using ObjectId = uint32_t;
using AssetId = uint32_t;

constexpr ObjectId INVALID_OBJECT_ID = 0xFFFFFFFF;
constexpr AssetId INVALID_ASSET_ID = 0xFFFFFFFF;

// A single object instance in the scene
struct SceneObject {
    ObjectId id = INVALID_OBJECT_ID;
    AssetId asset_id = INVALID_ASSET_ID;     // Reference to cached asset

    // Transform (column-major 4x4 matrix)
    float transform[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    // World-space bounding box (computed from asset bounds + transform)
    float bounds_min[3] = {0, 0, 0};
    float bounds_max[3] = {0, 0, 0};

    // State
    bool visible = true;
    bool selected = false;
    bool bounds_dirty = true;  // Needs recomputation after transform change

    // Optional name for debugging/UI
    std::string name;
};

// Draw command for a single cluster
struct DrawCommand {
    ObjectId object_id;
    uint32_t cluster_id;
    uint32_t meshlet_start;
    uint32_t meshlet_count;
    float screen_error;
    uint8_t lod_level;
    // Copy of the object's transform: a pointer into m_objects would
    // dangle as soon as an object is added or removed (vector reallocation)
    float transform[16];
};

// Scene statistics
struct SceneStats {
    uint32_t total_objects = 0;
    uint32_t visible_objects = 0;
    uint32_t total_clusters = 0;
    uint32_t visible_clusters = 0;
    uint32_t total_meshlets = 0;
    uint32_t visible_meshlets = 0;
    uint32_t unique_assets = 0;
    uint32_t instanced_objects = 0;  // Objects sharing an asset
};

// The scene graph - manages all objects and assets
class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    // Prevent copying (assets are heavy)
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // Move is OK
    Scene(Scene&&) = default;
    Scene& operator=(Scene&&) = default;

    // === Asset Management ===

    // Load an asset from file (returns asset ID, or INVALID_ASSET_ID on failure)
    // If already loaded, returns existing ID (asset cache)
    AssetId load_asset(const std::string& path);

    // Unload an asset (only if no objects reference it)
    bool unload_asset(AssetId asset_id);

    // Get asset by ID
    const VGeoAsset* get_asset(AssetId asset_id) const;

    // === Object Management ===

    // Add an object to the scene (returns object ID)
    ObjectId add_object(AssetId asset_id, const float* transform = nullptr, const char* name = nullptr);

    // Add an object by loading asset from path (convenience)
    ObjectId add_object_from_file(const std::string& path, const float* transform = nullptr, const char* name = nullptr);

    // Remove an object from the scene
    bool remove_object(ObjectId object_id);

    // Get object by ID
    SceneObject* get_object(ObjectId object_id);
    const SceneObject* get_object(ObjectId object_id) const;

    // Get all objects
    const std::vector<SceneObject>& objects() const { return m_objects; }

    // === Transform Operations ===

    // Set object transform (4x4 column-major matrix)
    void set_transform(ObjectId object_id, const float* transform);

    // Set object position (translation only)
    void set_position(ObjectId object_id, float x, float y, float z);

    // Set object rotation (Euler angles in radians)
    void set_rotation(ObjectId object_id, float rx, float ry, float rz);

    // Set object scale (uniform)
    void set_scale(ObjectId object_id, float scale);

    // Set object scale (non-uniform)
    void set_scale(ObjectId object_id, float sx, float sy, float sz);

    // === Visibility & Selection ===

    void set_visible(ObjectId object_id, bool visible);
    void set_selected(ObjectId object_id, bool selected);

    // === Rendering ===

    // Update scene for rendering (recompute bounds, etc.)
    void update();

    // Collect draw commands for visible geometry
    // Performs per-object frustum culling, then per-cluster LOD selection
    void collect_draw_commands(
        const Camera& camera,
        float error_threshold,
        std::vector<DrawCommand>& out_commands
    );

    // === Queries ===

    // Get scene statistics
    SceneStats get_stats() const;

    // Ray cast for object picking (returns object ID or INVALID_OBJECT_ID)
    ObjectId pick_object(const float* ray_origin, const float* ray_direction) const;

    // Get scene bounds (union of all object bounds)
    void get_bounds(float* out_min, float* out_max) const;

    // === Utility ===

    // Clear all objects and assets
    void clear();

    // Get number of objects
    size_t object_count() const { return m_objects.size(); }

    // Get number of loaded assets
    size_t asset_count() const { return m_assets.size(); }

private:
    // Recompute world bounds for an object
    void update_object_bounds(SceneObject& obj);

    // Find object index by ID
    int find_object_index(ObjectId object_id) const;

    // Asset cache (path -> asset ID)
    std::unordered_map<std::string, AssetId> m_asset_path_cache;

    // Loaded assets (asset ID -> asset data)
    std::unordered_map<AssetId, std::unique_ptr<VGeoAsset>> m_assets;

    // Asset reference counts (for unloading)
    std::unordered_map<AssetId, uint32_t> m_asset_refs;

    // Scene objects
    std::vector<SceneObject> m_objects;

    // ID generation
    ObjectId m_next_object_id = 1;
    AssetId m_next_asset_id = 1;
};

// === Matrix Utilities ===

namespace matrix {
    // Create identity matrix
    void identity(float* out);

    // Create translation matrix
    void translation(float* out, float x, float y, float z);

    // Create rotation matrix from Euler angles (radians)
    void rotation(float* out, float rx, float ry, float rz);

    // Create scale matrix
    void scale(float* out, float sx, float sy, float sz);

    // Multiply two 4x4 matrices: out = a * b
    void multiply(float* out, const float* a, const float* b);

    // Transform a point by a matrix
    void transform_point(float* out, const float* m, const float* p);

    // Transform an AABB by a matrix (computes new AABB)
    void transform_aabb(float* out_min, float* out_max,
                        const float* m,
                        const float* in_min, const float* in_max);
}

} // namespace vgeo
