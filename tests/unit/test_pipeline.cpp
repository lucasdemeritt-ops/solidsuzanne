// VGEO Pipeline Integration Test
// Tests the full preprocessing pipeline: OBJ -> Meshlets -> Hierarchy -> VGEO -> Load

#include "mesh_import.h"
#include "meshlet_gen.h"
#include "hierarchy.h"
#include "vgeo_writer.h"
#include "vgeo_loader.h"

#include <iostream>
#include <cstdio>
#include <cmath>

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

int main() {
    std::cout << "=== VGEO Pipeline Tests ===\n\n";

    RUN_TEST(obj_load);
    RUN_TEST(meshlet_gen_small);
    RUN_TEST(meshlet_gen_multiple);
    RUN_TEST(hierarchy_build);
    RUN_TEST(vgeo_roundtrip);
    RUN_TEST(icosphere_pipeline);
    RUN_TEST(bounding_sphere);

    std::cout << "\n=== Results ===\n";
    std::cout << "Tests run: " << tests_run << "\n";
    std::cout << "Tests failed: " << tests_failed << "\n";

    return tests_failed > 0 ? 1 : 0;
}
