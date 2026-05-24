// VGEO Pipeline Integration Test
// Tests the full preprocessing pipeline: OBJ -> Meshlets -> Hierarchy -> VGEO -> Load

#include "mesh_import.h"
#include "meshlet_gen.h"
#include "hierarchy.h"
#include "simplify.h"
#include "vgeo_writer.h"
#include "vgeo_loader.h"
#include "scene.h"

#include <iostream>
#include <cstdio>
#include <cmath>
#include <vector>

using namespace vgeo;

// Simple test framework
static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " #name "... "; \
    tests_run++; \
    try { \
        test_##name(); \
        std::cout << "PASSED\n"; \
    } catch (const std::exception& e) { \
        std::cout << "FAILED: " << e.what() << "\n"; \
        tests_failed++; \
    } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        throw std::runtime_error("Assertion failed: " #cond); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        throw std::runtime_error("Assertion failed: " #a " == " #b); \
    } \
} while(0)

// Test: Load OBJ file
TEST(obj_load) {
    RawMesh mesh;
    bool loaded = load_mesh("assets/cube.obj", mesh);
    ASSERT(loaded);
    ASSERT_EQ(mesh.vertex_count(), 24u);  // 6 faces * 4 vertices (not shared due to different normals)
    ASSERT_EQ(mesh.triangle_count(), 12u);
    ASSERT(!mesh.normals.empty());
    ASSERT(!mesh.uvs.empty());
}

// Test: Generate meshlets from small mesh
TEST(meshlet_gen_small) {
    RawMesh mesh;
    ASSERT(load_mesh("assets/cube.obj", mesh));

    MeshletParams params;
    params.max_vertices = 64;
    params.max_triangles = 126;

    MeshletData data;
    ASSERT(generate_meshlets(mesh, params, data));

    // Cube should fit in single meshlet
    ASSERT_EQ(data.meshlets.size(), 1u);
    ASSERT_EQ(data.bounds.size(), 1u);
    ASSERT_EQ(data.cones.size(), 1u);

    // Check vertex count matches
    ASSERT(data.meshlets[0].vertex_count <= params.max_vertices);
    ASSERT(data.meshlets[0].triangle_count <= params.max_triangles);
}

// Test: Generate meshlets with very small limits
TEST(meshlet_gen_multiple) {
    RawMesh mesh;
    ASSERT(load_mesh("assets/cube.obj", mesh));

    // Force multiple meshlets with tiny limits
    MeshletParams params;
    params.max_vertices = 4;
    params.max_triangles = 2;

    MeshletData data;
    ASSERT(generate_meshlets(mesh, params, data));

    // Should create multiple meshlets
    ASSERT(data.meshlets.size() > 1);

    // Verify all triangles are accounted for
    uint32_t total_tris = 0;
    for (const auto& m : data.meshlets) {
        total_tris += m.triangle_count;
        ASSERT(m.vertex_count <= params.max_vertices);
        ASSERT(m.triangle_count <= params.max_triangles);
    }
    ASSERT_EQ(total_tris, mesh.triangle_count());
}

// Test: Build hierarchy
TEST(hierarchy_build) {
    RawMesh mesh;
    ASSERT(load_mesh("assets/cube.obj", mesh));

    MeshletParams mp;
    MeshletData meshlets;
    ASSERT(generate_meshlets(mesh, mp, meshlets));

    HierarchyParams hp;
    hp.meshlets_per_cluster = 4;

    HierarchyData hierarchy;
    ASSERT(build_hierarchy(meshlets, mesh, hp, hierarchy));

    // Should have at least one cluster (root)
    ASSERT(!hierarchy.clusters.empty());

    // Find root
    bool has_root = false;
    for (const auto& c : hierarchy.clusters) {
        if (c.parent_id == INVALID_ID) {
            has_root = true;
            break;
        }
    }
    ASSERT(has_root);
}

// Test: Write and read VGEO file
TEST(vgeo_roundtrip) {
    const char* test_file = "test_output.vgeo";

    // Load mesh
    RawMesh mesh;
    ASSERT(load_mesh("assets/cube.obj", mesh));

    // Generate meshlets
    MeshletParams mp;
    MeshletData meshlets;
    ASSERT(generate_meshlets(mesh, mp, meshlets));

    // Build hierarchy
    HierarchyParams hp;
    HierarchyData hierarchy;
    ASSERT(build_hierarchy(meshlets, mesh, hp, hierarchy));

    // Write file
    WriteParams wp;
    ASSERT(write_vgeo(test_file, mesh, meshlets, hierarchy, wp));

    // Validate file
    std::string error;
    ASSERT(validate_vgeo(test_file, error));

    // Load file
    auto asset = load_vgeo(test_file);
    ASSERT(asset != nullptr);

    // Verify contents
    ASSERT_EQ(asset->header.meshlet_count, static_cast<uint32_t>(meshlets.meshlets.size()));
    ASSERT_EQ(asset->header.cluster_count, static_cast<uint32_t>(hierarchy.clusters.size()));
    ASSERT_EQ(asset->header.vertex_count, mesh.vertex_count());
    ASSERT(asset->header.flags & Flags::HAS_NORMALS);
    ASSERT(asset->header.flags & Flags::HAS_UVS);

    // Clean up
    std::remove(test_file);
}

// Test: Icosphere with multiple meshlets
TEST(icosphere_pipeline) {
    const char* test_file = "test_icosphere.vgeo";

    // Load mesh
    RawMesh mesh;
    ASSERT(load_mesh("assets/icosphere.obj", mesh));
    ASSERT(mesh.triangle_count() == 80);

    // Generate meshlets with smaller limits to get multiple
    MeshletParams mp;
    mp.max_vertices = 32;
    mp.max_triangles = 20;

    MeshletData meshlets;
    ASSERT(generate_meshlets(mesh, mp, meshlets));
    ASSERT(meshlets.meshlets.size() > 1);

    // Build hierarchy
    HierarchyParams hp;
    hp.meshlets_per_cluster = 2;

    HierarchyData hierarchy;
    ASSERT(build_hierarchy(meshlets, mesh, hp, hierarchy));

    // Write and reload
    WriteParams wp;
    ASSERT(write_vgeo(test_file, mesh, meshlets, hierarchy, wp));

    auto asset = load_vgeo(test_file);
    ASSERT(asset != nullptr);
    ASSERT_EQ(asset->meshlets.size(), meshlets.meshlets.size());

    // Clean up
    std::remove(test_file);
}

// Test: the INDEX_16BIT flag is never set while indices are written 32-bit
TEST(vgeo_index_flag_consistency) {
    const char* test_file = "test_idxflag.vgeo";

    RawMesh mesh;
    ASSERT(load_mesh("assets/icosphere.obj", mesh));
    MeshletParams mp;
    MeshletData meshlets;
    ASSERT(generate_meshlets(mesh, mp, meshlets));
    HierarchyParams hp;
    HierarchyData hierarchy;
    ASSERT(build_hierarchy(meshlets, mesh, hp, hierarchy));

    WriteParams wp;
    ASSERT(write_vgeo(test_file, mesh, meshlets, hierarchy, wp));

    auto asset = load_vgeo(test_file);
    ASSERT(asset != nullptr);

    // Writer always emits 32-bit indices, so this flag must stay clear.
    ASSERT(!(asset->header.flags & Flags::INDEX_16BIT));
    // The loaded 32-bit index buffer must match the header's count.
    ASSERT_EQ(asset->indices.size(), static_cast<size_t>(asset->header.index_count));

    std::remove(test_file);
}

// Test: Bounding sphere computation
TEST(bounding_sphere) {
    RawMesh mesh;
    ASSERT(load_mesh("assets/cube.obj", mesh));

    MeshletParams mp;
    MeshletData data;
    ASSERT(generate_meshlets(mesh, mp, data));

    // Check bounding sphere is reasonable for unit cube
    ASSERT(!data.bounds.empty());
    const BoundingSphere& bs = data.bounds[0];

    // Center should be near origin
    ASSERT(std::abs(bs.center[0]) < 1.0f);
    ASSERT(std::abs(bs.center[1]) < 1.0f);
    ASSERT(std::abs(bs.center[2]) < 1.0f);

    // Radius should be about sqrt(3)/2 for unit cube
    ASSERT(bs.radius > 0.5f);
    ASSERT(bs.radius < 1.5f);
}

// Decode an octahedral snorm16 normal back to a unit vector.
// Mirrors encode_octahedral() in vgeo_writer.cpp.
static void decode_octahedral(int16_t ix, int16_t iy, float& nx, float& ny, float& nz) {
    float ox = std::max(-1.0f, ix / 32767.0f);
    float oy = std::max(-1.0f, iy / 32767.0f);
    nx = ox;
    ny = oy;
    nz = 1.0f - std::abs(ox) - std::abs(oy);
    if (nz < 0.0f) {
        float tx = (1.0f - std::abs(oy)) * (nx >= 0.0f ? 1.0f : -1.0f);
        float ty = (1.0f - std::abs(ox)) * (ny >= 0.0f ? 1.0f : -1.0f);
        nx = tx;
        ny = ty;
    }
    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
}

// Build an N x N grid of quads in the XZ plane (procedural, no asset needed).
static RawMesh make_grid(uint32_t cells) {
    RawMesh m;
    uint32_t side = cells + 1;
    for (uint32_t z = 0; z < side; z++) {
        for (uint32_t x = 0; x < side; x++) {
            float fx = static_cast<float>(x) / cells - 0.5f;
            float fz = static_cast<float>(z) / cells - 0.5f;
            m.positions.push_back(fx);
            m.positions.push_back(0.0f);
            m.positions.push_back(fz);
        }
    }
    for (uint32_t z = 0; z < cells; z++) {
        for (uint32_t x = 0; x < cells; x++) {
            uint32_t a = z * side + x;
            uint32_t b = a + 1;
            uint32_t c = a + side;
            uint32_t d = c + 1;
            m.indices.push_back(a); m.indices.push_back(c); m.indices.push_back(b);
            m.indices.push_back(b); m.indices.push_back(c); m.indices.push_back(d);
        }
    }
    return m;
}

// Test: glTF 2.0 loader with embedded base64 buffer
TEST(gltf_load) {
    RawMesh mesh;
    ASSERT(load_mesh("assets/cube.gltf", mesh));
    ASSERT_EQ(mesh.vertex_count(), 8u);     // shared corners (no split normals)
    ASSERT_EQ(mesh.triangle_count(), 12u);
    ASSERT(!mesh.normals.empty());          // auto-computed when absent
}

// Test: octahedral normals survive the write -> load roundtrip.
// On a unit icosphere the smooth normal ~= the radial direction, so each
// decoded normal should align with its (normalized) position.
TEST(octahedral_roundtrip) {
    const char* test_file = "test_oct.vgeo";

    RawMesh mesh;
    ASSERT(load_mesh("assets/icosphere.obj", mesh));

    MeshletParams mp;
    MeshletData meshlets;
    ASSERT(generate_meshlets(mesh, mp, meshlets));

    HierarchyParams hp;
    HierarchyData hierarchy;
    ASSERT(build_hierarchy(meshlets, mesh, hp, hierarchy));

    WriteParams wp;
    ASSERT(write_vgeo(test_file, mesh, meshlets, hierarchy, wp));

    auto asset = load_vgeo(test_file);
    ASSERT(asset != nullptr);
    ASSERT(asset->header.flags & Flags::HAS_NORMALS);
    ASSERT_EQ(asset->normals.size(), asset->positions.size() / 3);

    for (size_t v = 0; v < asset->normals.size(); v++) {
        float nx, ny, nz;
        decode_octahedral(asset->normals[v].x, asset->normals[v].y, nx, ny, nz);

        // Decoded normal must be unit length.
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        ASSERT(std::abs(len - 1.0f) < 1e-3f);

        // And aligned with the outward radial direction.
        float px = asset->positions[v * 3 + 0];
        float py = asset->positions[v * 3 + 1];
        float pz = asset->positions[v * 3 + 2];
        float plen = std::sqrt(px * px + py * py + pz * pz);
        if (plen > 1e-6f) {
            float d = (nx * px + ny * py + nz * pz) / plen;
            ASSERT(d > 0.9f);
        }
    }

    std::remove(test_file);
}

// Test: multi-level hierarchy with deep nesting on a larger mesh
TEST(hierarchy_multilevel) {
    RawMesh mesh = make_grid(16);  // 512 triangles, 289 vertices
    ASSERT_EQ(mesh.triangle_count(), 512u);

    MeshletParams mp;
    mp.max_vertices = 16;
    mp.max_triangles = 8;
    MeshletData meshlets;
    ASSERT(generate_meshlets(mesh, mp, meshlets));
    ASSERT(meshlets.meshlets.size() > 8);

    HierarchyParams hp;
    hp.meshlets_per_cluster = 2;
    hp.branching_factor = 2;
    HierarchyData hierarchy;
    ASSERT(build_hierarchy(meshlets, mesh, hp, hierarchy));

    // Should produce a genuinely multi-level DAG.
    ASSERT(hierarchy.lod_levels >= 3);
    ASSERT(hierarchy.total_internal_clusters >= 1);
    ASSERT(!hierarchy.root_clusters.empty());

    // Structural integrity: every non-root cluster is listed in exactly its
    // parent's contiguous child range; every leaf carries meshlets.
    uint32_t n = static_cast<uint32_t>(hierarchy.clusters.size());
    for (uint32_t i = 0; i < n; i++) {
        const ClusterNode& c = hierarchy.clusters[i];
        if (c.parent_id != INVALID_ID) {
            ASSERT(c.parent_id < n);
            const ClusterNode& p = hierarchy.clusters[c.parent_id];
            ASSERT(i >= p.child_start);
            ASSERT(i < p.child_start + p.child_count);
        }
        if (c.child_count == 0) {
            ASSERT(c.meshlet_count > 0);  // leaf clusters reference geometry
        } else {
            ASSERT(c.child_start + c.child_count <= n);
        }
    }
}

// Test: QEM simplification reduces triangles on a smooth mesh
TEST(simplify_reduces) {
    RawMesh mesh;
    ASSERT(load_mesh("assets/icosphere.obj", mesh));
    ASSERT_EQ(mesh.triangle_count(), 80u);

    SimplifyParams sp;
    sp.target_ratio = 0.5f;
    RawMesh out;
    SimplifyResult res;
    ASSERT(simplify_mesh(mesh, sp, out, res));

    // Strictly fewer triangles, but still a real mesh.
    ASSERT(res.output_triangles < res.input_triangles);
    ASSERT(res.output_triangles >= 1u);
    ASSERT(res.output_triangles <= 56u);          // ~30%+ reduction achieved
    ASSERT_EQ(out.triangle_count(), res.output_triangles);
    ASSERT(out.vertex_count() < mesh.vertex_count());
    ASSERT(res.error >= 0.0f);
    ASSERT(std::isfinite(res.error));

    // Output normals are present and unit length.
    ASSERT_EQ(out.normals.size(), out.positions.size());
    for (size_t v = 0; v < out.vertex_count(); v++) {
        float nx = out.normals[v * 3 + 0];
        float ny = out.normals[v * 3 + 1];
        float nz = out.normals[v * 3 + 2];
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        ASSERT(std::abs(len - 1.0f) < 1e-2f);
    }
}

// Test: absolute target triangle count is respected (within a small margin)
TEST(simplify_target_count) {
    RawMesh mesh;
    ASSERT(load_mesh("assets/icosphere.obj", mesh));

    SimplifyParams sp;
    sp.target_triangle_count = 20;
    RawMesh out;
    SimplifyResult res;
    ASSERT(simplify_mesh(mesh, sp, out, res));

    // Greedy collapse stops at the first count <= target, so we never go far
    // below the target; flip rejection may leave us slightly above it.
    ASSERT(res.output_triangles <= 28u);
    ASSERT(res.output_triangles >= 16u);
}

// Test: more aggressive simplification yields larger geometric error
TEST(simplify_error_monotonic) {
    RawMesh mesh;
    ASSERT(load_mesh("assets/icosphere.obj", mesh));

    SimplifyParams none;
    none.target_ratio = 1.0f;  // keep everything -> no collapses
    RawMesh out_none; SimplifyResult res_none;
    ASSERT(simplify_mesh(mesh, none, out_none, res_none));
    ASSERT(res_none.error == 0.0f);
    ASSERT_EQ(res_none.output_triangles, mesh.triangle_count());

    SimplifyParams mild; mild.target_ratio = 0.75f;
    RawMesh out_mild; SimplifyResult res_mild;
    ASSERT(simplify_mesh(mesh, mild, out_mild, res_mild));

    SimplifyParams hard; hard.target_ratio = 0.25f;
    RawMesh out_hard; SimplifyResult res_hard;
    ASSERT(simplify_mesh(mesh, hard, out_hard, res_hard));

    ASSERT(res_hard.output_triangles < res_mild.output_triangles);
    ASSERT(res_hard.error >= res_mild.error);
}

// Test: simplifying a watertight cube stays valid (no crash / no garbage)
TEST(simplify_cube_valid) {
    RawMesh mesh;
    ASSERT(load_mesh("assets/cube.gltf", mesh));  // 8 shared verts, watertight

    SimplifyParams sp;
    sp.target_triangle_count = 4;
    RawMesh out;
    SimplifyResult res;
    ASSERT(simplify_mesh(mesh, sp, out, res));

    ASSERT(res.output_triangles >= 1u);
    ASSERT(res.output_triangles <= mesh.triangle_count());
    ASSERT(std::isfinite(res.error));

    // No index may reference a vertex outside the compacted buffer.
    for (uint32_t idx : out.indices) {
        ASSERT(idx < out.vertex_count());
    }
    // No degenerate triangles remain.
    for (size_t t = 0; t < out.indices.size(); t += 3) {
        uint32_t a = out.indices[t + 0];
        uint32_t b = out.indices[t + 1];
        uint32_t c = out.indices[t + 2];
        ASSERT(a != b && b != c && a != c);
    }
}

// Test: empty / triangleless input is rejected cleanly
TEST(simplify_rejects_empty) {
    RawMesh empty;
    SimplifyParams sp;
    RawMesh out;
    SimplifyResult res;
    ASSERT(!simplify_mesh(empty, sp, out, res));
}

// Build a cube .vgeo on disk so the scene tests have a real asset to load.
static bool build_cube_vgeo(const char* path) {
    RawMesh mesh;
    if (!load_mesh("assets/cube.obj", mesh)) return false;
    MeshletParams mp;
    MeshletData meshlets;
    if (!generate_meshlets(mesh, mp, meshlets)) return false;
    HierarchyParams hp;
    HierarchyData hierarchy;
    if (!build_hierarchy(meshlets, mesh, hp, hierarchy)) return false;
    WriteParams wp;
    return write_vgeo(path, mesh, meshlets, hierarchy, wp);
}

// Test: scene matrix utilities
TEST(scene_matrix_utils) {
    float id[16];
    matrix::identity(id);
    ASSERT(id[0] == 1.0f && id[5] == 1.0f && id[10] == 1.0f && id[15] == 1.0f);
    ASSERT(id[1] == 0.0f && id[12] == 0.0f);

    float t[16];
    matrix::translation(t, 1.0f, 2.0f, 3.0f);
    ASSERT(t[12] == 1.0f && t[13] == 2.0f && t[14] == 3.0f);

    // identity * T == T
    float prod[16];
    matrix::multiply(prod, id, t);
    for (int i = 0; i < 16; i++) {
        ASSERT(std::abs(prod[i] - t[i]) < 1e-6f);
    }

    // Translate the origin.
    float p[3] = {0, 0, 0};
    float tp[3];
    matrix::transform_point(tp, t, p);
    ASSERT(std::abs(tp[0] - 1.0f) < 1e-6f);
    ASSERT(std::abs(tp[1] - 2.0f) < 1e-6f);
    ASSERT(std::abs(tp[2] - 3.0f) < 1e-6f);

    // Translate a unit box by +10 in x.
    float bmin[3] = {-0.5f, -0.5f, -0.5f};
    float bmax[3] = {0.5f, 0.5f, 0.5f};
    float shift[16];
    matrix::translation(shift, 10.0f, 0.0f, 0.0f);
    float omin[3], omax[3];
    matrix::transform_aabb(omin, omax, shift, bmin, bmax);
    ASSERT(std::abs(omin[0] - 9.5f) < 1e-4f);
    ASSERT(std::abs(omax[0] - 10.5f) < 1e-4f);
    ASSERT(std::abs(omin[1] + 0.5f) < 1e-4f);
}

// Test: asset caching / instancing — same path loads once, shared by objects
TEST(scene_instancing) {
    const char* asset_path = "scene_cube.vgeo";
    ASSERT(build_cube_vgeo(asset_path));

    Scene scene;
    AssetId a0 = scene.load_asset(asset_path);
    AssetId a1 = scene.load_asset(asset_path);  // cache hit
    ASSERT(a0 != INVALID_ASSET_ID);
    ASSERT_EQ(a0, a1);
    ASSERT_EQ(scene.asset_count(), 1u);

    ObjectId o0 = scene.add_object(a0);
    ObjectId o1 = scene.add_object(a0);
    ASSERT(o0 != INVALID_OBJECT_ID && o1 != INVALID_OBJECT_ID);
    ASSERT(o0 != o1);
    ASSERT_EQ(scene.object_count(), 2u);

    SceneStats stats = scene.get_stats();
    ASSERT_EQ(stats.total_objects, 2u);
    ASSERT_EQ(stats.unique_assets, 1u);
    ASSERT_EQ(stats.instanced_objects, 2u);  // both objects share one asset

    std::remove(asset_path);
}

// Test: object add / lookup / removal lifecycle
TEST(scene_object_lifecycle) {
    const char* asset_path = "scene_cube2.vgeo";
    ASSERT(build_cube_vgeo(asset_path));

    Scene scene;
    AssetId a = scene.load_asset(asset_path);
    ASSERT(a != INVALID_ASSET_ID);

    // Adding against a bogus asset id fails cleanly.
    ASSERT_EQ(scene.add_object(12345u), INVALID_OBJECT_ID);

    ObjectId o = scene.add_object(a, nullptr, "cube");
    ASSERT(o != INVALID_OBJECT_ID);
    ASSERT(scene.get_object(o) != nullptr);
    ASSERT_EQ(scene.object_count(), 1u);

    ASSERT(scene.remove_object(o));
    ASSERT(scene.get_object(o) == nullptr);
    ASSERT_EQ(scene.object_count(), 0u);
    ASSERT(!scene.remove_object(o));  // already gone

    std::remove(asset_path);
}

// Test: object world bounds follow the transform
TEST(scene_transform_bounds) {
    const char* asset_path = "scene_cube3.vgeo";
    ASSERT(build_cube_vgeo(asset_path));

    Scene scene;
    ObjectId o = scene.add_object_from_file(asset_path);
    ASSERT(o != INVALID_OBJECT_ID);

    scene.set_position(o, 5.0f, 0.0f, 0.0f);
    scene.update();

    const SceneObject* obj = scene.get_object(o);
    ASSERT(obj != nullptr);
    // Unit cube [-0.5,0.5] translated by +5 in x.
    ASSERT(std::abs(obj->bounds_min[0] - 4.5f) < 1e-3f);
    ASSERT(std::abs(obj->bounds_max[0] - 5.5f) < 1e-3f);

    float smin[3], smax[3];
    scene.get_bounds(smin, smax);
    ASSERT(std::abs(smin[0] - 4.5f) < 1e-3f);
    ASSERT(std::abs(smax[0] - 5.5f) < 1e-3f);

    std::remove(asset_path);
}

int main() {
    std::cout << "=== VGEO Pipeline Tests ===\n\n";

    RUN_TEST(obj_load);
    RUN_TEST(meshlet_gen_small);
    RUN_TEST(meshlet_gen_multiple);
    RUN_TEST(hierarchy_build);
    RUN_TEST(vgeo_roundtrip);
    RUN_TEST(icosphere_pipeline);
    RUN_TEST(vgeo_index_flag_consistency);
    RUN_TEST(bounding_sphere);
    RUN_TEST(gltf_load);
    RUN_TEST(octahedral_roundtrip);
    RUN_TEST(hierarchy_multilevel);
    RUN_TEST(simplify_reduces);
    RUN_TEST(simplify_target_count);
    RUN_TEST(simplify_error_monotonic);
    RUN_TEST(simplify_cube_valid);
    RUN_TEST(simplify_rejects_empty);
    RUN_TEST(scene_matrix_utils);
    RUN_TEST(scene_instancing);
    RUN_TEST(scene_object_lifecycle);
    RUN_TEST(scene_transform_bounds);

    std::cout << "\n=== Results ===\n";
    std::cout << "Tests run: " << tests_run << "\n";
    std::cout << "Tests failed: " << tests_failed << "\n";

    return tests_failed > 0 ? 1 : 0;
}
