#pragma once

#include "mesh_import.h"
#include <cstdint>
#include <limits>

namespace vgeo {

// Quadric error metric (QEM) mesh simplification.
//
// Implements Garland-Heckbert edge-collapse decimation: each vertex accumulates
// the squared-distance-to-plane quadrics of its incident faces, edges are
// collapsed cheapest-first, and the surviving vertex is moved to the position
// that minimizes the combined quadric. Boundary edges are protected and
// collapses that flip a face normal are rejected.
//
// This produces coarse LOD geometry for the cluster hierarchy. It operates only
// on positions and topology; normals are recomputed for the output and UVs (if
// present) are carried from the surviving endpoint of each collapse.

struct SimplifyParams {
    // Fraction of input triangles to KEEP (0..1). Ignored when
    // target_triangle_count > 0.
    float target_ratio = 0.5f;

    // Absolute target triangle count. When > 0 this overrides target_ratio.
    uint32_t target_triangle_count = 0;

    // Stop collapsing once the cheapest remaining collapse would introduce a
    // geometric error (length units) above this bound, even if the triangle
    // target has not been reached.
    float max_error = std::numeric_limits<float>::max();

    // Add heavy perpendicular-plane quadrics along open boundary edges so the
    // silhouette of non-watertight meshes is preserved.
    bool preserve_borders = true;

    // Reject collapses that would flip the orientation of an adjacent face.
    bool prevent_flips = true;
};

struct SimplifyResult {
    // Largest geometric error introduced by any collapse, in mesh length units
    // (sqrt of the Garland-Heckbert quadric cost). Suitable as a LOD error
    // metric. Zero when no collapse was performed.
    float error = 0.0f;

    uint32_t input_triangles = 0;
    uint32_t output_triangles = 0;
    uint32_t input_vertices = 0;
    uint32_t output_vertices = 0;
};

// Simplify a triangle mesh. Returns false only on invalid input (no triangles).
// On success `out` holds the decimated mesh with recomputed normals; `out` may
// alias neither nullptr nor `in`.
bool simplify_mesh(
    const RawMesh& in,
    const SimplifyParams& params,
    RawMesh& out,
    SimplifyResult& result
);

} // namespace vgeo
