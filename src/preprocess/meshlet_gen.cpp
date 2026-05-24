// VGEO Meshlet Generation
// Build meshlets from raw mesh using a greedy algorithm
// TODO: Integrate meshoptimizer for production use

#include "meshlet_gen.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <unordered_map>
#include <limits>

namespace vgeo {

// Helper: compute bounding sphere from a set of vertex positions
static BoundingSphere compute_bounding_sphere(
    const std::vector<float>& positions,
    const std::vector<uint32_t>& vertex_indices
) {
    BoundingSphere sphere = {};

    if (vertex_indices.empty()) {
        return sphere;
    }

    // First pass: compute centroid
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (uint32_t idx : vertex_indices) {
        cx += positions[idx * 3 + 0];
        cy += positions[idx * 3 + 1];
        cz += positions[idx * 3 + 2];
    }
    float inv_count = 1.0f / static_cast<float>(vertex_indices.size());
    cx *= inv_count;
    cy *= inv_count;
    cz *= inv_count;

    sphere.center[0] = cx;
    sphere.center[1] = cy;
    sphere.center[2] = cz;

    // Second pass: find maximum distance from centroid
    float max_dist_sq = 0.0f;
    for (uint32_t idx : vertex_indices) {
        float dx = positions[idx * 3 + 0] - cx;
        float dy = positions[idx * 3 + 1] - cy;
        float dz = positions[idx * 3 + 2] - cz;
        float dist_sq = dx * dx + dy * dy + dz * dz;
        max_dist_sq = std::max(max_dist_sq, dist_sq);
    }

    sphere.radius = std::sqrt(max_dist_sq);
    return sphere;
}

// Helper: compute normal cone from triangle normals
static NormalCone compute_normal_cone(
    const std::vector<float>& /*normals*/,  // cone is derived from face geometry
    const std::vector<float>& positions,
    const std::vector<uint32_t>& indices,
    uint32_t index_start,
    uint32_t triangle_count
) {
    NormalCone cone = {};

    if (triangle_count == 0) {
        cone.axis[0] = 0;
        cone.axis[1] = 127;
        cone.axis[2] = 0;
        cone.cos_angle = 127;  // cos(0) = 1
        return cone;
    }

    // Compute average normal (cone axis)
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    for (uint32_t t = 0; t < triangle_count; t++) {
        uint32_t i0 = indices[index_start + t * 3 + 0];
        uint32_t i1 = indices[index_start + t * 3 + 1];
        uint32_t i2 = indices[index_start + t * 3 + 2];

        // Compute face normal
        float p0x = positions[i0 * 3 + 0], p0y = positions[i0 * 3 + 1], p0z = positions[i0 * 3 + 2];
        float p1x = positions[i1 * 3 + 0], p1y = positions[i1 * 3 + 1], p1z = positions[i1 * 3 + 2];
        float p2x = positions[i2 * 3 + 0], p2y = positions[i2 * 3 + 1], p2z = positions[i2 * 3 + 2];

        float e1x = p1x - p0x, e1y = p1y - p0y, e1z = p1z - p0z;
        float e2x = p2x - p0x, e2y = p2y - p0y, e2z = p2z - p0z;

        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;

        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) {
            ax += nx / len;
            ay += ny / len;
            az += nz / len;
        }
    }

    // Normalize axis
    float axis_len = std::sqrt(ax * ax + ay * ay + az * az);
    if (axis_len > 1e-8f) {
        ax /= axis_len;
        ay /= axis_len;
        az /= axis_len;
    } else {
        ax = 0.0f;
        ay = 1.0f;
        az = 0.0f;
    }

    // Find minimum dot product (maximum angle from axis)
    float min_dot = 1.0f;
    for (uint32_t t = 0; t < triangle_count; t++) {
        uint32_t i0 = indices[index_start + t * 3 + 0];
        uint32_t i1 = indices[index_start + t * 3 + 1];
        uint32_t i2 = indices[index_start + t * 3 + 2];

        float p0x = positions[i0 * 3 + 0], p0y = positions[i0 * 3 + 1], p0z = positions[i0 * 3 + 2];
        float p1x = positions[i1 * 3 + 0], p1y = positions[i1 * 3 + 1], p1z = positions[i1 * 3 + 2];
        float p2x = positions[i2 * 3 + 0], p2y = positions[i2 * 3 + 1], p2z = positions[i2 * 3 + 2];

        float e1x = p1x - p0x, e1y = p1y - p0y, e1z = p1z - p0z;
        float e2x = p2x - p0x, e2y = p2y - p0y, e2z = p2z - p0z;

        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;

        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) {
            nx /= len;
            ny /= len;
            nz /= len;
            float dot = ax * nx + ay * ny + az * nz;
            min_dot = std::min(min_dot, dot);
        }
    }

    // Convert to snorm8
    cone.axis[0] = static_cast<int8_t>(std::clamp(ax * 127.0f, -127.0f, 127.0f));
    cone.axis[1] = static_cast<int8_t>(std::clamp(ay * 127.0f, -127.0f, 127.0f));
    cone.axis[2] = static_cast<int8_t>(std::clamp(az * 127.0f, -127.0f, 127.0f));
    cone.cos_angle = static_cast<int8_t>(std::clamp(min_dot * 127.0f, -127.0f, 127.0f));

    return cone;
}

// Greedy meshlet generation algorithm
// This is a simple implementation - meshoptimizer would be more optimal
bool generate_meshlets(
    const RawMesh& mesh,
    const MeshletParams& params,
    MeshletData& out_data
) {
    out_data.vertex_indices.clear();
    out_data.local_indices.clear();
    out_data.meshlets.clear();
    out_data.bounds.clear();
    out_data.cones.clear();

    if (mesh.indices.empty() || mesh.positions.empty()) {
        return true;  // Empty mesh is valid
    }

    const uint32_t max_vertices = params.max_vertices;
    const uint32_t max_triangles = params.max_triangles;
    const uint32_t triangle_count = static_cast<uint32_t>(mesh.indices.size() / 3);

    // Track which triangles have been assigned to a meshlet
    std::vector<bool> triangle_used(triangle_count, false);

    // Build adjacency: for each vertex, list triangles that use it
    std::unordered_map<uint32_t, std::vector<uint32_t>> vertex_to_triangles;
    for (uint32_t t = 0; t < triangle_count; t++) {
        vertex_to_triangles[mesh.indices[t * 3 + 0]].push_back(t);
        vertex_to_triangles[mesh.indices[t * 3 + 1]].push_back(t);
        vertex_to_triangles[mesh.indices[t * 3 + 2]].push_back(t);
    }

    uint32_t triangles_remaining = triangle_count;
    uint32_t current_vertex_offset = 0;
    uint32_t current_index_offset = 0;

    while (triangles_remaining > 0) {
        // Start a new meshlet
        std::vector<uint32_t> meshlet_vertices;
        std::unordered_map<uint32_t, uint8_t> vertex_to_local;
        std::vector<uint8_t> meshlet_local_indices;
        uint32_t meshlet_triangle_count = 0;

        // Find an unused triangle to seed the meshlet
        uint32_t seed_triangle = UINT32_MAX;
        for (uint32_t t = 0; t < triangle_count; t++) {
            if (!triangle_used[t]) {
                seed_triangle = t;
                break;
            }
        }

        if (seed_triangle == UINT32_MAX) {
            break;  // No more triangles
        }

        // Add seed triangle
        auto add_triangle = [&](uint32_t t) -> bool {
            if (triangle_used[t]) return false;
            if (meshlet_triangle_count >= max_triangles) return false;

            uint32_t v0 = mesh.indices[t * 3 + 0];
            uint32_t v1 = mesh.indices[t * 3 + 1];
            uint32_t v2 = mesh.indices[t * 3 + 2];

            // Count how many new vertices this triangle would add
            uint32_t new_verts = 0;
            if (vertex_to_local.find(v0) == vertex_to_local.end()) new_verts++;
            if (vertex_to_local.find(v1) == vertex_to_local.end()) new_verts++;
            if (vertex_to_local.find(v2) == vertex_to_local.end()) new_verts++;

            if (meshlet_vertices.size() + new_verts > max_vertices) {
                return false;  // Would exceed vertex limit
            }

            // Add vertices
            auto add_vertex = [&](uint32_t v) -> uint8_t {
                auto it = vertex_to_local.find(v);
                if (it != vertex_to_local.end()) {
                    return it->second;
                }
                uint8_t local_idx = static_cast<uint8_t>(meshlet_vertices.size());
                vertex_to_local[v] = local_idx;
                meshlet_vertices.push_back(v);
                return local_idx;
            };

            uint8_t l0 = add_vertex(v0);
            uint8_t l1 = add_vertex(v1);
            uint8_t l2 = add_vertex(v2);

            meshlet_local_indices.push_back(l0);
            meshlet_local_indices.push_back(l1);
            meshlet_local_indices.push_back(l2);

            triangle_used[t] = true;
            meshlet_triangle_count++;
            triangles_remaining--;

            return true;
        };

        add_triangle(seed_triangle);

        // Greedily add adjacent triangles first
        bool added = true;
        while (added && meshlet_triangle_count < max_triangles && meshlet_vertices.size() < max_vertices) {
            added = false;

            // Find candidate triangles adjacent to current meshlet vertices
            // Prefer triangles that share more vertices with current meshlet
            uint32_t best_triangle = UINT32_MAX;
            uint32_t best_shared = 0;
            uint32_t best_new_verts = 4;  // More than possible

            for (uint32_t v : meshlet_vertices) {
                for (uint32_t t : vertex_to_triangles[v]) {
                    if (triangle_used[t]) continue;

                    uint32_t tv0 = mesh.indices[t * 3 + 0];
                    uint32_t tv1 = mesh.indices[t * 3 + 1];
                    uint32_t tv2 = mesh.indices[t * 3 + 2];

                    uint32_t shared = 0;
                    uint32_t new_verts = 0;

                    if (vertex_to_local.find(tv0) != vertex_to_local.end()) shared++;
                    else new_verts++;
                    if (vertex_to_local.find(tv1) != vertex_to_local.end()) shared++;
                    else new_verts++;
                    if (vertex_to_local.find(tv2) != vertex_to_local.end()) shared++;
                    else new_verts++;

                    // Prefer triangles with more shared vertices, fewer new vertices
                    if (shared > best_shared || (shared == best_shared && new_verts < best_new_verts)) {
                        if (meshlet_vertices.size() + new_verts <= max_vertices) {
                            best_triangle = t;
                            best_shared = shared;
                            best_new_verts = new_verts;
                        }
                    }
                }
            }

            if (best_triangle != UINT32_MAX) {
                if (add_triangle(best_triangle)) {
                    added = true;
                }
            }
        }

        // If there's still capacity, add any remaining triangles (even non-adjacent)
        // This ensures small meshes fit in a single meshlet
        if (meshlet_triangle_count < max_triangles && meshlet_vertices.size() < max_vertices) {
            for (uint32_t t = 0; t < triangle_count && meshlet_triangle_count < max_triangles; t++) {
                if (triangle_used[t]) continue;

                uint32_t tv0 = mesh.indices[t * 3 + 0];
                uint32_t tv1 = mesh.indices[t * 3 + 1];
                uint32_t tv2 = mesh.indices[t * 3 + 2];

                // Count new vertices needed
                uint32_t new_verts = 0;
                if (vertex_to_local.find(tv0) == vertex_to_local.end()) new_verts++;
                if (vertex_to_local.find(tv1) == vertex_to_local.end()) new_verts++;
                if (vertex_to_local.find(tv2) == vertex_to_local.end()) new_verts++;

                // Add if there's room
                if (meshlet_vertices.size() + new_verts <= max_vertices) {
                    add_triangle(t);
                }
            }
        }

        // Finalize meshlet
        MeshletDescriptor desc;
        desc.vertex_offset = current_vertex_offset;
        desc.vertex_count = static_cast<uint32_t>(meshlet_vertices.size());
        desc.index_offset = current_index_offset;
        desc.triangle_count = meshlet_triangle_count;
        desc.cluster_id = 0;  // Will be set by hierarchy builder

        out_data.meshlets.push_back(desc);

        // Add vertices and local indices to global arrays
        for (uint32_t v : meshlet_vertices) {
            out_data.vertex_indices.push_back(v);
        }
        for (uint8_t li : meshlet_local_indices) {
            out_data.local_indices.push_back(li);
        }

        // Compute bounding sphere
        BoundingSphere sphere = compute_bounding_sphere(mesh.positions, meshlet_vertices);
        out_data.bounds.push_back(sphere);

        // Compute normal cone
        NormalCone cone = compute_normal_cone(
            mesh.normals,
            mesh.positions,
            mesh.indices,
            seed_triangle * 3,  // Approximate - use actual meshlet triangles
            meshlet_triangle_count
        );
        out_data.cones.push_back(cone);

        current_vertex_offset += desc.vertex_count;
        current_index_offset += meshlet_triangle_count * 3;
    }

    return true;
}

} // namespace vgeo
