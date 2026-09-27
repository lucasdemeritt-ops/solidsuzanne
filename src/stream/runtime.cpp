// .vgeo v2 runtime: pick the DAG cut for the current views and hand it out
// chunk by chunk as small indexed meshes.

#include "vgeo_stream.h"
#include "format_v2.h"
#include "io_util.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Asset {
    std::vector<uint8_t> blob;
    vgeo2::Header h;
    const float* positions = nullptr;
    const float* normals = nullptr;
    const float* uvs = nullptr;
    const uint16_t* vmat = nullptr;
    const uint32_t* indices = nullptr;
    const vgeo2::Cluster* clusters = nullptr;
    const vgeo2::Group* groups = nullptr;
    const vgeo2::Chunk* chunks = nullptr;
    const uint32_t* chunk_clusters = nullptr;
    std::vector<std::string> materials;

    // selection state
    std::vector<uint8_t> group_pass;
    std::vector<uint8_t> selected;

    // extraction scratch
    std::vector<uint32_t> stamp;
    std::vector<int32_t> local;
    uint32_t stamp_id = 0;
    std::vector<float> out_pos, out_nrm, out_uv;
    std::vector<int32_t> out_corner, out_mat, out_lod;
};

void set_err(char* err, int err_len, const std::string& msg) {
    if (err && err_len > 0) std::snprintf(err, size_t(err_len), "%s", msg.c_str());
}

bool section_ok(const Asset& a, uint64_t off, uint64_t bytes) {
    return off >= sizeof(vgeo2::Header) && off <= a.blob.size() && bytes <= a.blob.size() - off;
}

// projected error as a fraction of view height; FLT_MAX error never passes
inline bool group_passes(const vgeo2::Group& g, const vgeo_view* views, int view_count) {
    if (!(g.error < FLT_MAX)) return false;
    for (int i = 0; i < view_count; ++i) {
        const vgeo_view& v = views[i];
        float e;
        if (v.ortho) {
            e = g.error / std::max(v.ortho_height, 1e-20f);
        } else {
            float dx = g.center[0] - v.camera[0], dy = g.center[1] - v.camera[1], dz = g.center[2] - v.camera[2];
            float d = std::sqrt(dx * dx + dy * dy + dz * dz) - g.radius;
            e = g.error / std::max(d, v.znear) * (v.proj * 0.5f);
        }
        if (e > v.threshold) return false;
    }
    return true;
}

inline bool sphere_visible(const float* c, float r, const vgeo_view* views, int view_count) {
    bool any_frustum = false;
    for (int i = 0; i < view_count; ++i) {
        const vgeo_view& v = views[i];
        if (!v.use_frustum) return true;  // a view without culling keeps everything
        any_frustum = true;
        bool inside = true;
        for (int p = 0; p < 6 && inside; ++p)
            inside = v.planes[p][0] * c[0] + v.planes[p][1] * c[1] + v.planes[p][2] * c[2] + v.planes[p][3] >= -r;
        if (inside) return true;
    }
    return !any_frustum;
}

inline uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

void signatures(Asset& a, uint64_t* chunk_sig, vgeo_cut_stats* stats) {
    uint64_t tris = 0;
    uint32_t count = 0;
    for (uint32_t c = 0; c < a.h.chunk_count; ++c) {
        const vgeo2::Chunk& ch = a.chunks[c];
        uint64_t sig = 0x243F6A8885A308D3ull;
        for (uint32_t k = 0; k < ch.cluster_count; ++k) {
            uint32_t id = a.chunk_clusters[ch.cluster_offset + k];
            if (a.selected[id]) {
                sig += mix(id);  // order independent
                tris += a.clusters[id].tri_count;
                ++count;
            }
        }
        if (chunk_sig) chunk_sig[c] = sig;
    }
    if (stats) {
        stats->triangles = tris;
        stats->clusters = count;
        stats->changed_chunks = 0;
    }
}

}  // namespace

extern "C" VGEO_API void* vgeo_open(const char* path_utf8, char* err, int err_len) {
    if (!path_utf8) { set_err(err, err_len, "no path"); return nullptr; }
    FILE* f = vgeo_io::open_utf8(path_utf8, "rb");
    if (!f) { set_err(err, err_len, std::string("cannot open ") + path_utf8); return nullptr; }
    Asset* a = new Asset();
    std::fseek(f, 0, SEEK_END);
    int64_t size = vgeo_io::tell(f);
    vgeo_io::seek(f, 0);
    if (size < int64_t(sizeof(vgeo2::Header))) {
        std::fclose(f); delete a;
        set_err(err, err_len, "file too small");
        return nullptr;
    }
    try {
        a->blob.resize(size_t(size));
    } catch (...) {
        std::fclose(f); delete a;
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    size_t got = std::fread(a->blob.data(), 1, size_t(size), f);
    std::fclose(f);
    if (got != size_t(size)) { delete a; set_err(err, err_len, "read failed"); return nullptr; }

    std::memcpy(&a->h, a->blob.data(), sizeof(vgeo2::Header));
    const vgeo2::Header& h = a->h;
    if (std::memcmp(h.magic, vgeo2::kMagic, 8) != 0) { delete a; set_err(err, err_len, "not a VGEO v2 file"); return nullptr; }
    if (h.version != vgeo2::kVersion) { delete a; set_err(err, err_len, "unsupported VGEO version"); return nullptr; }
    bool ok = h.file_size <= a->blob.size()
        && section_ok(*a, h.off_positions, uint64_t(h.vertex_count) * 12)
        && section_ok(*a, h.off_normals, uint64_t(h.vertex_count) * 12)
        && (!(h.flags & vgeo2::kHasUVs) || section_ok(*a, h.off_uvs, uint64_t(h.vertex_count) * 8))
        && section_ok(*a, h.off_vmat, uint64_t(h.vertex_count) * 2)
        && section_ok(*a, h.off_indices, uint64_t(h.index_count) * 4)
        && section_ok(*a, h.off_clusters, uint64_t(h.cluster_count) * sizeof(vgeo2::Cluster))
        && section_ok(*a, h.off_groups, uint64_t(h.group_count) * sizeof(vgeo2::Group))
        && section_ok(*a, h.off_chunks, uint64_t(h.chunk_count) * sizeof(vgeo2::Chunk))
        && section_ok(*a, h.off_chunk_clusters, uint64_t(h.chunk_cluster_count) * 4)
        && section_ok(*a, h.off_materials, 4);
    if (!ok) { delete a; set_err(err, err_len, "corrupt file (section out of range)"); return nullptr; }

    const uint8_t* b = a->blob.data();
    a->positions = reinterpret_cast<const float*>(b + h.off_positions);
    a->normals = reinterpret_cast<const float*>(b + h.off_normals);
    a->uvs = (h.flags & vgeo2::kHasUVs) ? reinterpret_cast<const float*>(b + h.off_uvs) : nullptr;
    a->vmat = reinterpret_cast<const uint16_t*>(b + h.off_vmat);
    a->indices = reinterpret_cast<const uint32_t*>(b + h.off_indices);
    a->clusters = reinterpret_cast<const vgeo2::Cluster*>(b + h.off_clusters);
    a->groups = reinterpret_cast<const vgeo2::Group*>(b + h.off_groups);
    a->chunks = reinterpret_cast<const vgeo2::Chunk*>(b + h.off_chunks);
    a->chunk_clusters = reinterpret_cast<const uint32_t*>(b + h.off_chunk_clusters);

    // validate every reference once so the hot paths can trust the data
    for (uint32_t i = 0; i < h.cluster_count && ok; ++i) {
        const vgeo2::Cluster& c = a->clusters[i];
        ok = uint64_t(c.index_offset) + uint64_t(c.tri_count) * 3 <= h.index_count
            && c.group >= 0 && uint32_t(c.group) < h.group_count
            && c.refined >= -1 && c.refined < int32_t(h.group_count)
            && c.chunk < h.chunk_count;
    }
    for (uint32_t i = 0; i < h.index_count && ok; ++i) ok = a->indices[i] < h.vertex_count;
    for (uint32_t i = 0; i < h.chunk_count && ok; ++i)
        ok = uint64_t(a->chunks[i].cluster_offset) + a->chunks[i].cluster_count <= h.chunk_cluster_count;
    for (uint32_t i = 0; i < h.chunk_cluster_count && ok; ++i) ok = a->chunk_clusters[i] < h.cluster_count;
    if (!ok) { delete a; set_err(err, err_len, "corrupt file (bad reference)"); return nullptr; }

    {
        const uint8_t* p = b + h.off_materials;
        const uint8_t* end = b + a->blob.size();
        uint32_t n;
        std::memcpy(&n, p, 4);
        p += 4;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t len;
            if (end - p < 4) break;
            std::memcpy(&len, p, 4);
            p += 4;
            if (uint64_t(end - p) < len) break;
            a->materials.emplace_back(reinterpret_cast<const char*>(p), len);
            p += len;
        }
    }

    a->group_pass.assign(h.group_count, 0);
    a->selected.assign(h.cluster_count, 0);
    a->stamp.assign(h.vertex_count, 0);
    a->local.assign(h.vertex_count, 0);
    return a;
}

extern "C" VGEO_API void vgeo_close(void* handle) { delete static_cast<Asset*>(handle); }

extern "C" VGEO_API int vgeo_get_info(void* handle, vgeo_info* info) {
    Asset* a = static_cast<Asset*>(handle);
    if (!a || !info) return 1;
    info->vertex_count = a->h.vertex_count;
    info->cluster_count = a->h.cluster_count;
    info->group_count = a->h.group_count;
    info->chunk_count = a->h.chunk_count;
    info->material_count = uint32_t(a->materials.size());
    info->lod_levels = a->h.lod_levels;
    info->flags = a->h.flags;
    info->source_triangles = a->h.source_triangles;
    std::memcpy(info->aabb_min, a->h.aabb_min, sizeof(info->aabb_min));
    std::memcpy(info->aabb_max, a->h.aabb_max, sizeof(info->aabb_max));
    return 0;
}

extern "C" VGEO_API int vgeo_material_name(void* handle, uint32_t index, char* buf, int buf_len) {
    Asset* a = static_cast<Asset*>(handle);
    if (!a || index >= a->materials.size() || !buf || buf_len <= 0) return 1;
    std::snprintf(buf, size_t(buf_len), "%s", a->materials[index].c_str());
    return 0;
}

extern "C" VGEO_API int vgeo_select(void* handle, const vgeo_view* views, int view_count,
                                    uint64_t* chunk_sig, vgeo_cut_stats* stats) {
    Asset* a = static_cast<Asset*>(handle);
    if (!a || !views || view_count <= 0) return 1;
    for (uint32_t g = 0; g < a->h.group_count; ++g)
        a->group_pass[g] = group_passes(a->groups[g], views, view_count) ? 1 : 0;
    for (uint32_t i = 0; i < a->h.cluster_count; ++i) {
        const vgeo2::Cluster& c = a->clusters[i];
        bool in_cut = !a->group_pass[c.group] && (c.refined < 0 || a->group_pass[c.refined]);
        a->selected[i] = (in_cut && sphere_visible(c.center, c.radius, views, view_count)) ? 1 : 0;
    }
    signatures(*a, chunk_sig, stats);
    return 0;
}

extern "C" VGEO_API int vgeo_select_level(void* handle, int depth, uint64_t* chunk_sig, vgeo_cut_stats* stats) {
    Asset* a = static_cast<Asset*>(handle);
    if (!a) return 1;
    for (uint32_t i = 0; i < a->h.cluster_count; ++i) {
        const vgeo2::Cluster& c = a->clusters[i];
        bool terminal = !(a->groups[c.group].error < FLT_MAX);
        bool sel = depth < 0 ? terminal : (int(c.depth) == depth || (int(c.depth) < depth && terminal));
        a->selected[i] = sel ? 1 : 0;
    }
    signatures(*a, chunk_sig, stats);
    return 0;
}

extern "C" VGEO_API int vgeo_extract(void* handle, uint32_t chunk, vgeo_chunk_data* out) {
    Asset* a = static_cast<Asset*>(handle);
    if (!a || !out || chunk >= a->h.chunk_count) return 1;
    const vgeo2::Chunk& ch = a->chunks[chunk];

    if (++a->stamp_id == 0) {  // wrapped: reset stamps
        std::fill(a->stamp.begin(), a->stamp.end(), 0);
        a->stamp_id = 1;
    }
    a->out_pos.clear(); a->out_nrm.clear(); a->out_uv.clear();
    a->out_corner.clear(); a->out_mat.clear(); a->out_lod.clear();

    int32_t next = 0;
    for (uint32_t k = 0; k < ch.cluster_count; ++k) {
        uint32_t id = a->chunk_clusters[ch.cluster_offset + k];
        if (!a->selected[id]) continue;
        const vgeo2::Cluster& c = a->clusters[id];
        const uint32_t* idx = a->indices + c.index_offset;
        for (uint32_t t = 0; t < c.tri_count; ++t) {
            for (int j = 0; j < 3; ++j) {
                uint32_t v = idx[t * 3 + j];
                if (a->stamp[v] != a->stamp_id) {
                    a->stamp[v] = a->stamp_id;
                    a->local[v] = next++;
                    a->out_pos.insert(a->out_pos.end(), a->positions + v * 3, a->positions + v * 3 + 3);
                    a->out_nrm.insert(a->out_nrm.end(), a->normals + v * 3, a->normals + v * 3 + 3);
                    if (a->uvs) a->out_uv.insert(a->out_uv.end(), a->uvs + v * 2, a->uvs + v * 2 + 2);
                }
                a->out_corner.push_back(a->local[v]);
            }
            a->out_mat.push_back(int32_t(a->vmat[idx[t * 3]]));
            a->out_lod.push_back(int32_t(c.depth));
        }
    }
    out->vertex_count = uint32_t(next);
    out->tri_count = uint32_t(a->out_mat.size());
    out->positions = a->out_pos.data();
    out->normals = a->out_nrm.data();
    out->uvs = a->uvs ? a->out_uv.data() : nullptr;
    out->corner_verts = a->out_corner.data();
    out->face_materials = a->out_mat.data();
    out->face_lod = a->out_lod.data();
    return 0;
}
