// VGEO Mesh Simplification
// Quadric error metric (QEM) edge-collapse decimation (Garland-Heckbert 1997).

#include "simplify.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vgeo {

namespace {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

static Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double length(const Vec3& a) { return std::sqrt(dot(a, a)); }

// Symmetric 4x4 quadric stored as its 10 unique upper-triangle coefficients.
// Indexing matches the matrix:
//   [ a  b  c  d ]
//   [ b  e  f  g ]
//   [ c  f  h  i ]
//   [ d  g  i  j ]
struct Quadric {
    double a = 0, b = 0, c = 0, d = 0;
    double e = 0, f = 0, g = 0;
    double h = 0, i = 0;
    double j = 0;

    Quadric& operator+=(const Quadric& q) {
        a += q.a; b += q.b; c += q.c; d += q.d;
        e += q.e; f += q.f; g += q.g;
        h += q.h; i += q.i;
        j += q.j;
        return *this;
    }
};

// Build the fundamental quadric for a plane (nx,ny,nz,off) with unit normal.
static Quadric plane_quadric(const Vec3& n, double off, double weight) {
    Quadric q;
    q.a = weight * n.x * n.x;
    q.b = weight * n.x * n.y;
    q.c = weight * n.x * n.z;
    q.d = weight * n.x * off;
    q.e = weight * n.y * n.y;
    q.f = weight * n.y * n.z;
    q.g = weight * n.y * off;
    q.h = weight * n.z * n.z;
    q.i = weight * n.z * off;
    q.j = weight * off * off;
    return q;
}

// Evaluate v^T Q v for the homogeneous point (p, 1).
static double quadric_error(const Quadric& q, const Vec3& p) {
    // x*(a x + b y + c z + d) + y*(b x + e y + f z + g)
    // + z*(c x + f y + h z + i) + (d x + g y + i z + j)
    return p.x * (q.a * p.x + q.b * p.y + q.c * p.z + q.d)
         + p.y * (q.b * p.x + q.e * p.y + q.f * p.z + q.g)
         + p.z * (q.c * p.x + q.f * p.y + q.h * p.z + q.i)
         + (q.d * p.x + q.g * p.y + q.i * p.z + q.j);
}

// Solve the 3x3 system from the quadric for the cost-minimizing position.
// Returns false if the matrix is near-singular.
static bool optimal_position(const Quadric& q, Vec3& out) {
    // A = top-left 3x3, b = -(d,g,i)
    double a11 = q.a, a12 = q.b, a13 = q.c;
    double a22 = q.e, a23 = q.f;
    double a33 = q.h;

    double det =
        a11 * (a22 * a33 - a23 * a23) -
        a12 * (a12 * a33 - a23 * a13) +
        a13 * (a12 * a23 - a22 * a13);

    if (std::abs(det) < 1e-12) {
        return false;
    }

    double inv_det = 1.0 / det;
    // Cofactor (adjugate) of symmetric A.
    double c11 = (a22 * a33 - a23 * a23);
    double c12 = -(a12 * a33 - a23 * a13);
    double c13 = (a12 * a23 - a22 * a13);
    double c22 = (a11 * a33 - a13 * a13);
    double c23 = -(a11 * a23 - a12 * a13);
    double c33 = (a11 * a22 - a12 * a12);

    double bx = -q.d, by = -q.g, bz = -q.i;
    out.x = inv_det * (c11 * bx + c12 * by + c13 * bz);
    out.y = inv_det * (c12 * bx + c22 * by + c23 * bz);
    out.z = inv_det * (c13 * bx + c23 * by + c33 * bz);
    return true;
}

struct Edge {
    double cost;
    uint32_t v0, v1;
    uint32_t ver0, ver1; // versions at insertion, for lazy invalidation
    Vec3 target;
    bool operator<(const Edge& o) const { return cost > o.cost; } // min-heap
};

struct Simplifier {
    std::vector<Vec3> pos;
    std::vector<Quadric> quad;
    std::vector<bool> alive;
    std::vector<uint32_t> version;

    // Faces as triples of vertex indices; dead faces have any index == kDead.
    std::vector<std::array<uint32_t, 3>> faces;
    std::vector<bool> face_alive;

    // Per-vertex incident face lists.
    std::vector<std::vector<uint32_t>> incident;

    std::priority_queue<Edge> heap;
    uint32_t live_tris = 0;
    bool prevent_flips = true;

    Vec3 face_normal(const std::array<uint32_t, 3>& f, bool& degenerate) const {
        Vec3 e1 = sub(pos[f[1]], pos[f[0]]);
        Vec3 e2 = sub(pos[f[2]], pos[f[0]]);
        Vec3 n = cross(e1, e2);
        double len = length(n);
        degenerate = (len < 1e-20);
        if (degenerate) return {0, 0, 0};
        return {n.x / len, n.y / len, n.z / len};
    }

    // Choose the collapse target for edge (v0,v1) and its cost.
    void evaluate(uint32_t v0, uint32_t v1, double& cost, Vec3& target) const {
        Quadric q = quad[v0];
        q += quad[v1];

        Vec3 opt;
        if (optimal_position(q, opt)) {
            target = opt;
        } else {
            // Fall back to the cheapest of the two endpoints and the midpoint.
            Vec3 mid{(pos[v0].x + pos[v1].x) * 0.5,
                     (pos[v0].y + pos[v1].y) * 0.5,
                     (pos[v0].z + pos[v1].z) * 0.5};
            double c0 = quadric_error(q, pos[v0]);
            double c1 = quadric_error(q, pos[v1]);
            double cm = quadric_error(q, mid);
            target = pos[v0]; double best = c0;
            if (c1 < best) { best = c1; target = pos[v1]; }
            if (cm < best) { best = cm; target = mid; }
        }
        cost = quadric_error(q, target);
        if (cost < 0.0) cost = 0.0; // numerical guard
    }

    // Would moving `moved` to `target` flip any incident face not on the edge?
    bool causes_flip(uint32_t moved, uint32_t other, const Vec3& target) const {
        for (uint32_t fi : incident[moved]) {
            if (!face_alive[fi]) continue;
            const auto& f = faces[fi];
            // Skip faces that contain the edge being collapsed; they vanish.
            if (f[0] == other || f[1] == other || f[2] == other) continue;

            bool degen_before = false;
            Vec3 n_before = face_normal(f, degen_before);
            if (degen_before) continue;

            std::array<Vec3, 3> p{pos[f[0]], pos[f[1]], pos[f[2]]};
            for (int k = 0; k < 3; ++k) {
                if (f[k] == moved) p[k] = target;
            }
            Vec3 e1 = sub(p[1], p[0]);
            Vec3 e2 = sub(p[2], p[0]);
            Vec3 n_after = cross(e1, e2);
            double len = length(n_after);
            if (len < 1e-20) return true; // collapsed to a sliver
            n_after = {n_after.x / len, n_after.y / len, n_after.z / len};
            if (dot(n_before, n_after) < 0.1) return true; // > ~84deg flip
        }
        return false;
    }

    void push_edge(uint32_t v0, uint32_t v1) {
        if (v0 == v1) return;
        if (!alive[v0] || !alive[v1]) return;
        double cost;
        Vec3 target;
        evaluate(v0, v1, cost, target);
        if (prevent_flips &&
            (causes_flip(v0, v1, target) || causes_flip(v1, v0, target))) {
            return; // skip illegal collapse; it simply won't be offered
        }
        heap.push(Edge{cost, v0, v1, version[v0], version[v1], target});
    }

    // Enumerate the one-ring neighbours of v through its live faces.
    void neighbours(uint32_t v, std::vector<uint32_t>& out) const {
        out.clear();
        for (uint32_t fi : incident[v]) {
            if (!face_alive[fi]) continue;
            for (uint32_t w : faces[fi]) {
                if (w != v) out.push_back(w);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
};

} // namespace

bool simplify_mesh(
    const RawMesh& in,
    const SimplifyParams& params,
    RawMesh& out,
    SimplifyResult& result
) {
    const uint32_t in_verts = in.vertex_count();
    const uint32_t in_tris = in.triangle_count();

    result = SimplifyResult{};
    result.input_triangles = in_tris;
    result.input_vertices = in_verts;

    if (in_tris == 0 || in_verts == 0) {
        return false;
    }

    // Determine the target triangle count.
    uint32_t target_tris;
    if (params.target_triangle_count > 0) {
        target_tris = params.target_triangle_count;
    } else {
        float ratio = params.target_ratio;
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        target_tris = static_cast<uint32_t>(std::lround(in_tris * ratio));
    }
    if (target_tris < 1) target_tris = 1;

    Simplifier s;
    s.prevent_flips = params.prevent_flips;
    s.pos.resize(in_verts);
    for (uint32_t v = 0; v < in_verts; ++v) {
        s.pos[v] = {in.positions[v * 3 + 0], in.positions[v * 3 + 1], in.positions[v * 3 + 2]};
    }
    s.quad.assign(in_verts, Quadric{});
    s.alive.assign(in_verts, true);
    s.version.assign(in_verts, 0);
    s.incident.assign(in_verts, {});

    s.faces.reserve(in_tris);
    s.face_alive.assign(in_tris, true);

    // Build faces, per-face quadrics, and incidence.
    for (uint32_t t = 0; t < in_tris; ++t) {
        std::array<uint32_t, 3> f{in.indices[t * 3 + 0], in.indices[t * 3 + 1], in.indices[t * 3 + 2]};
        uint32_t fi = static_cast<uint32_t>(s.faces.size());
        s.faces.push_back(f);

        bool degen = false;
        Vec3 n = s.face_normal(f, degen);
        if (!degen) {
            double off = -dot(n, s.pos[f[0]]);
            Quadric q = plane_quadric(n, off, 1.0);
            s.quad[f[0]] += q;
            s.quad[f[1]] += q;
            s.quad[f[2]] += q;
        }
        for (uint32_t v : f) s.incident[v].push_back(fi);
    }
    s.live_tris = in_tris;

    // Boundary preservation: an edge with exactly one incident face is open.
    // Add a heavy quadric for the plane through the edge perpendicular to the
    // face so the silhouette resists collapse.
    if (params.preserve_borders) {
        std::unordered_map<uint64_t, int> edge_count;
        edge_count.reserve(in_tris * 3);
        auto key = [](uint32_t a, uint32_t b) {
            if (a > b) std::swap(a, b);
            return (static_cast<uint64_t>(a) << 32) | b;
        };
        for (const auto& f : s.faces) {
            edge_count[key(f[0], f[1])]++;
            edge_count[key(f[1], f[2])]++;
            edge_count[key(f[2], f[0])]++;
        }

        // Bounding-box diagonal sets the boundary penalty scale.
        Vec3 bmin = s.pos[0], bmax = s.pos[0];
        for (const auto& p : s.pos) {
            bmin.x = std::min(bmin.x, p.x); bmax.x = std::max(bmax.x, p.x);
            bmin.y = std::min(bmin.y, p.y); bmax.y = std::max(bmax.y, p.y);
            bmin.z = std::min(bmin.z, p.z); bmax.z = std::max(bmax.z, p.z);
        }
        double diag = length(sub(bmax, bmin));
        double penalty = (diag > 0.0) ? diag * diag * 100.0 : 100.0;

        for (uint32_t fi = 0; fi < s.faces.size(); ++fi) {
            const auto& f = s.faces[fi];
            bool degen = false;
            Vec3 fn = s.face_normal(f, degen);
            if (degen) continue;
            const int ek[3][2] = {{0, 1}, {1, 2}, {2, 0}};
            for (auto& e : ek) {
                uint32_t a = f[e[0]], b = f[e[1]];
                if (edge_count[key(a, b)] != 1) continue; // not a boundary edge
                Vec3 edge_dir = sub(s.pos[b], s.pos[a]);
                Vec3 plane_n = cross(edge_dir, fn); // perpendicular to edge, in-plane
                double len = length(plane_n);
                if (len < 1e-20) continue;
                plane_n = {plane_n.x / len, plane_n.y / len, plane_n.z / len};
                double off = -dot(plane_n, s.pos[a]);
                Quadric q = plane_quadric(plane_n, off, penalty);
                s.quad[a] += q;
                s.quad[b] += q;
            }
        }
    }

    // Seed the heap with every unique edge.
    {
        std::unordered_map<uint64_t, char> seen;
        seen.reserve(in_tris * 3);
        auto key = [](uint32_t a, uint32_t b) {
            if (a > b) std::swap(a, b);
            return (static_cast<uint64_t>(a) << 32) | b;
        };
        for (const auto& f : s.faces) {
            const int ek[3][2] = {{0, 1}, {1, 2}, {2, 0}};
            for (auto& e : ek) {
                uint32_t a = f[e[0]], b = f[e[1]];
                if (seen.emplace(key(a, b), 1).second) {
                    s.push_edge(a, b);
                }
            }
        }
    }

    double max_cost = 0.0;
    std::vector<uint32_t> nb;

    // Greedy collapse loop.
    while (s.live_tris > target_tris && !s.heap.empty()) {
        Edge edge = s.heap.top();
        s.heap.pop();

        uint32_t v0 = edge.v0, v1 = edge.v1;
        if (!s.alive[v0] || !s.alive[v1]) continue;
        if (edge.ver0 != s.version[v0] || edge.ver1 != s.version[v1]) continue; // stale

        double err = std::sqrt(edge.cost);
        if (err > params.max_error) {
            break; // cheapest remaining collapse already exceeds the bound
        }

        // Perform the collapse: v1 merges into v0 at edge.target.
        s.pos[v0] = edge.target;
        s.quad[v0] += s.quad[v1];
        s.alive[v1] = false;

        // Rewrite faces incident to v1; drop faces that become degenerate.
        for (uint32_t fi : s.incident[v1]) {
            if (!s.face_alive[fi]) continue;
            auto& f = s.faces[fi];
            for (auto& idx : f) {
                if (idx == v1) idx = v0;
            }
            if (f[0] == f[1] || f[1] == f[2] || f[0] == f[2]) {
                s.face_alive[fi] = false;
                if (s.live_tris > 0) s.live_tris--;
            } else {
                s.incident[v0].push_back(fi);
            }
        }
        s.incident[v1].clear();

        // Drop dead faces from v0's incidence to keep it from growing unbounded.
        {
            auto& inc = s.incident[v0];
            inc.erase(std::remove_if(inc.begin(), inc.end(),
                        [&](uint32_t fi) { return !s.face_alive[fi]; }),
                      inc.end());
            std::sort(inc.begin(), inc.end());
            inc.erase(std::unique(inc.begin(), inc.end()), inc.end());
        }

        s.version[v0]++;
        s.version[v1]++;
        if (edge.cost > max_cost) max_cost = edge.cost;

        // Re-price edges around the merged vertex.
        s.neighbours(v0, nb);
        for (uint32_t w : nb) {
            s.push_edge(v0, w);
        }
    }

    // Compact surviving vertices and faces into the output mesh.
    bool has_uvs = !in.uvs.empty();
    std::vector<uint32_t> remap(in_verts, UINT32_MAX);
    out.positions.clear();
    out.normals.clear();
    out.uvs.clear();
    out.indices.clear();

    for (uint32_t fi = 0; fi < s.faces.size(); ++fi) {
        if (!s.face_alive[fi]) continue;
        for (uint32_t v : s.faces[fi]) {
            if (remap[v] == UINT32_MAX) {
                remap[v] = static_cast<uint32_t>(out.positions.size() / 3);
                out.positions.push_back(static_cast<float>(s.pos[v].x));
                out.positions.push_back(static_cast<float>(s.pos[v].y));
                out.positions.push_back(static_cast<float>(s.pos[v].z));
                if (has_uvs) {
                    out.uvs.push_back(in.uvs[v * 2 + 0]);
                    out.uvs.push_back(in.uvs[v * 2 + 1]);
                }
            }
            out.indices.push_back(remap[v]);
        }
    }

    // Recompute smooth vertex normals for the decimated mesh.
    out.normals.assign(out.positions.size(), 0.0f);
    for (size_t t = 0; t < out.indices.size(); t += 3) {
        uint32_t i0 = out.indices[t + 0], i1 = out.indices[t + 1], i2 = out.indices[t + 2];
        Vec3 p0{out.positions[i0 * 3 + 0], out.positions[i0 * 3 + 1], out.positions[i0 * 3 + 2]};
        Vec3 p1{out.positions[i1 * 3 + 0], out.positions[i1 * 3 + 1], out.positions[i1 * 3 + 2]};
        Vec3 p2{out.positions[i2 * 3 + 0], out.positions[i2 * 3 + 1], out.positions[i2 * 3 + 2]};
        Vec3 n = cross(sub(p1, p0), sub(p2, p0));
        for (uint32_t idx : {i0, i1, i2}) {
            out.normals[idx * 3 + 0] += static_cast<float>(n.x);
            out.normals[idx * 3 + 1] += static_cast<float>(n.y);
            out.normals[idx * 3 + 2] += static_cast<float>(n.z);
        }
    }
    for (size_t v = 0; v < out.normals.size() / 3; ++v) {
        float nx = out.normals[v * 3 + 0], ny = out.normals[v * 3 + 1], nz = out.normals[v * 3 + 2];
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) {
            out.normals[v * 3 + 0] = nx / len;
            out.normals[v * 3 + 1] = ny / len;
            out.normals[v * 3 + 2] = nz / len;
        } else {
            out.normals[v * 3 + 0] = 0.0f;
            out.normals[v * 3 + 1] = 1.0f;
            out.normals[v * 3 + 2] = 0.0f;
        }
    }

    result.error = static_cast<float>(std::sqrt(max_cost));
    result.output_triangles = out.triangle_count();
    result.output_vertices = out.vertex_count();
    return true;
}

} // namespace vgeo
