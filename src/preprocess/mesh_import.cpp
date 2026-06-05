// VGEO Mesh Import
// Load OBJ, glTF and GLB files and extract geometry data

#include "mesh_import.h"
#include "gltf_import.h"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace vgeo {

// Helper to compute hash for vertex deduplication
struct VertexKey {
    int pos_idx;
    int norm_idx;
    int uv_idx;

    bool operator==(const VertexKey& other) const {
        return pos_idx == other.pos_idx &&
               norm_idx == other.norm_idx &&
               uv_idx == other.uv_idx;
    }
};

struct VertexKeyHash {
    size_t operator()(const VertexKey& k) const {
        return std::hash<int>()(k.pos_idx) ^
               (std::hash<int>()(k.norm_idx) << 1) ^
               (std::hash<int>()(k.uv_idx) << 2);
    }
};

// Parse an OBJ face vertex index (can be v, v/vt, v/vt/vn, or v//vn)
static bool parse_face_vertex(const std::string& token, int& pos_idx, int& uv_idx, int& norm_idx) {
    pos_idx = 0;
    uv_idx = 0;
    norm_idx = 0;

    size_t first_slash = token.find('/');
    if (first_slash == std::string::npos) {
        // Just position index
        pos_idx = std::stoi(token);
        return true;
    }

    pos_idx = std::stoi(token.substr(0, first_slash));

    size_t second_slash = token.find('/', first_slash + 1);
    if (second_slash == std::string::npos) {
        // v/vt format
        if (first_slash + 1 < token.length()) {
            uv_idx = std::stoi(token.substr(first_slash + 1));
        }
        return true;
    }

    // v/vt/vn or v//vn format
    if (second_slash > first_slash + 1) {
        uv_idx = std::stoi(token.substr(first_slash + 1, second_slash - first_slash - 1));
    }
    if (second_slash + 1 < token.length()) {
        norm_idx = std::stoi(token.substr(second_slash + 1));
    }

    return true;
}

// Compute face normal from three vertices
static void compute_face_normal(
    const std::vector<float>& positions,
    uint32_t i0, uint32_t i1, uint32_t i2,
    float& nx, float& ny, float& nz
) {
    float ax = positions[i0 * 3 + 0];
    float ay = positions[i0 * 3 + 1];
    float az = positions[i0 * 3 + 2];

    float bx = positions[i1 * 3 + 0];
    float by = positions[i1 * 3 + 1];
    float bz = positions[i1 * 3 + 2];

    float cx = positions[i2 * 3 + 0];
    float cy = positions[i2 * 3 + 1];
    float cz = positions[i2 * 3 + 2];

    // Edge vectors
    float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
    float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;

    // Cross product
    nx = e1y * e2z - e1z * e2y;
    ny = e1z * e2x - e1x * e2z;
    nz = e1x * e2y - e1y * e2x;

    // Normalize
    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) {
        nx /= len;
        ny /= len;
        nz /= len;
    } else {
        nx = 0.0f;
        ny = 1.0f;
        nz = 0.0f;
    }
}

// Load OBJ file
static bool load_obj(const std::string& path, RawMesh& out_mesh) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    // Temporary storage for OBJ data (1-indexed in OBJ format)
    std::vector<float> temp_positions;
    std::vector<float> temp_normals;
    std::vector<float> temp_uvs;

    // Face indices as vertex keys
    std::vector<VertexKey> face_vertices;

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream iss(line);
        std::string prefix;
        iss >> prefix;

        if (prefix == "v") {
            // Vertex position
            float x, y, z;
            iss >> x >> y >> z;
            temp_positions.push_back(x);
            temp_positions.push_back(y);
            temp_positions.push_back(z);
        }
        else if (prefix == "vn") {
            // Vertex normal
            float x, y, z;
            iss >> x >> y >> z;
            temp_normals.push_back(x);
            temp_normals.push_back(y);
            temp_normals.push_back(z);
        }
        else if (prefix == "vt") {
            // Texture coordinate
            float u, v;
            iss >> u >> v;
            temp_uvs.push_back(u);
            temp_uvs.push_back(v);
        }
        else if (prefix == "f") {
            // Face - collect all vertices first
            std::vector<VertexKey> poly_verts;
            std::string token;

            while (iss >> token) {
                int pos_idx, uv_idx, norm_idx;
                if (!parse_face_vertex(token, pos_idx, uv_idx, norm_idx)) {
                    return false;
                }

                // Convert from 1-indexed to 0-indexed, handle negative indices
                int pos_count = static_cast<int>(temp_positions.size() / 3);
                int norm_count = static_cast<int>(temp_normals.size() / 3);
                int uv_count = static_cast<int>(temp_uvs.size() / 2);

                if (pos_idx < 0) pos_idx = pos_count + pos_idx + 1;
                if (norm_idx < 0) norm_idx = norm_count + norm_idx + 1;
                if (uv_idx < 0) uv_idx = uv_count + uv_idx + 1;

                VertexKey key;
                key.pos_idx = pos_idx - 1;  // Convert to 0-indexed
                key.norm_idx = (norm_idx > 0) ? norm_idx - 1 : -1;
                key.uv_idx = (uv_idx > 0) ? uv_idx - 1 : -1;

                poly_verts.push_back(key);
            }

            // Triangulate polygon (fan triangulation)
            for (size_t i = 2; i < poly_verts.size(); i++) {
                face_vertices.push_back(poly_verts[0]);
                face_vertices.push_back(poly_verts[i - 1]);
                face_vertices.push_back(poly_verts[i]);
            }
        }
    }

    // Build deduplicated vertex buffer
    std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertex_map;

    out_mesh.positions.clear();
    out_mesh.normals.clear();
    out_mesh.uvs.clear();
    out_mesh.indices.clear();

    bool has_normals = !temp_normals.empty();
    bool has_uvs = !temp_uvs.empty();

    for (const VertexKey& key : face_vertices) {
        auto it = vertex_map.find(key);
        if (it != vertex_map.end()) {
            // Reuse existing vertex
            out_mesh.indices.push_back(it->second);
        } else {
            // Create new vertex
            uint32_t new_idx = static_cast<uint32_t>(out_mesh.positions.size() / 3);
            vertex_map[key] = new_idx;
            out_mesh.indices.push_back(new_idx);

            // Copy position
            if (key.pos_idx >= 0 && key.pos_idx * 3 + 2 < static_cast<int>(temp_positions.size())) {
                out_mesh.positions.push_back(temp_positions[key.pos_idx * 3 + 0]);
                out_mesh.positions.push_back(temp_positions[key.pos_idx * 3 + 1]);
                out_mesh.positions.push_back(temp_positions[key.pos_idx * 3 + 2]);
            } else {
                out_mesh.positions.push_back(0.0f);
                out_mesh.positions.push_back(0.0f);
                out_mesh.positions.push_back(0.0f);
            }

            // Copy or default normal
            if (has_normals && key.norm_idx >= 0 && key.norm_idx * 3 + 2 < static_cast<int>(temp_normals.size())) {
                out_mesh.normals.push_back(temp_normals[key.norm_idx * 3 + 0]);
                out_mesh.normals.push_back(temp_normals[key.norm_idx * 3 + 1]);
                out_mesh.normals.push_back(temp_normals[key.norm_idx * 3 + 2]);
            } else {
                // Placeholder - will be computed later if needed
                out_mesh.normals.push_back(0.0f);
                out_mesh.normals.push_back(1.0f);
                out_mesh.normals.push_back(0.0f);
            }

            // Copy or default UV
            if (has_uvs && key.uv_idx >= 0 && key.uv_idx * 2 + 1 < static_cast<int>(temp_uvs.size())) {
                out_mesh.uvs.push_back(temp_uvs[key.uv_idx * 2 + 0]);
                out_mesh.uvs.push_back(temp_uvs[key.uv_idx * 2 + 1]);
            } else {
                out_mesh.uvs.push_back(0.0f);
                out_mesh.uvs.push_back(0.0f);
            }
        }
    }

    // If we didn't have normals, compute face normals and assign to vertices
    if (!has_normals) {
        // Accumulate face normals at vertices
        std::vector<float> accum_normals(out_mesh.positions.size(), 0.0f);

        for (size_t i = 0; i < out_mesh.indices.size(); i += 3) {
            uint32_t i0 = out_mesh.indices[i + 0];
            uint32_t i1 = out_mesh.indices[i + 1];
            uint32_t i2 = out_mesh.indices[i + 2];

            float nx, ny, nz;
            compute_face_normal(out_mesh.positions, i0, i1, i2, nx, ny, nz);

            // Accumulate at each vertex of the triangle
            accum_normals[i0 * 3 + 0] += nx;
            accum_normals[i0 * 3 + 1] += ny;
            accum_normals[i0 * 3 + 2] += nz;

            accum_normals[i1 * 3 + 0] += nx;
            accum_normals[i1 * 3 + 1] += ny;
            accum_normals[i1 * 3 + 2] += nz;

            accum_normals[i2 * 3 + 0] += nx;
            accum_normals[i2 * 3 + 1] += ny;
            accum_normals[i2 * 3 + 2] += nz;
        }

        // Normalize accumulated normals
        out_mesh.normals.resize(out_mesh.positions.size());
        for (size_t i = 0; i < out_mesh.positions.size() / 3; i++) {
            float nx = accum_normals[i * 3 + 0];
            float ny = accum_normals[i * 3 + 1];
            float nz = accum_normals[i * 3 + 2];

            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-8f) {
                out_mesh.normals[i * 3 + 0] = nx / len;
                out_mesh.normals[i * 3 + 1] = ny / len;
                out_mesh.normals[i * 3 + 2] = nz / len;
            } else {
                out_mesh.normals[i * 3 + 0] = 0.0f;
                out_mesh.normals[i * 3 + 1] = 1.0f;
                out_mesh.normals[i * 3 + 2] = 0.0f;
            }
        }
    }

    return true;
}

// Check file extension (case-insensitive)
static bool has_extension(const std::string& path, const std::string& ext) {
    if (path.length() < ext.length()) return false;
    std::string path_ext = path.substr(path.length() - ext.length());
    std::transform(path_ext.begin(), path_ext.end(), path_ext.begin(), ::tolower);
    return path_ext == ext;
}

bool load_mesh(const std::string& path, RawMesh& out_mesh) {
    if (has_extension(path, ".obj")) {
        return load_obj(path, out_mesh);
    }

    if (has_extension(path, ".gltf") || has_extension(path, ".glb")) {
        return load_gltf(path, out_mesh);
    }

    // Unsupported format
    return false;
}

} // namespace vgeo
