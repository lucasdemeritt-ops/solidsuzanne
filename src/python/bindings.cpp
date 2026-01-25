// VGEO Python Bindings
// Exposes scene graph and rendering to Python for Blender integration

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include "scene.h"
#include "vgeo_loader.h"
#include "camera.h"

#include <memory>
#include <vector>
#include <cstring>

namespace py = pybind11;

namespace vgeo {

// Simplified camera for Python
struct PyCamera {
    float position[3] = {0, 0, 10};
    float target[3] = {0, 0, 0};
    float fov = 45.0f;
    float near_plane = 0.1f;
    float far_plane = 10000.0f;
    float aspect = 16.0f / 9.0f;

    // Internal camera for actual rendering
    Camera camera;

    void update() {
        std::memcpy(camera.position, position, sizeof(position));
        std::memcpy(camera.target, target, sizeof(target));
        camera.fov = fov;
        camera.near_plane = near_plane;
        camera.far_plane = far_plane;
        camera.aspect = aspect;
        camera.update();
    }

    void set_position(float x, float y, float z) {
        position[0] = x; position[1] = y; position[2] = z;
    }

    void set_target(float x, float y, float z) {
        target[0] = x; target[1] = y; target[2] = z;
    }
};

// Python-facing scene wrapper
class PyScene {
public:
    PyScene() = default;

    // Load a .vgeo asset and return its ID
    int load_asset(const std::string& path) {
        AssetId id = m_scene.load_asset(path);
        return static_cast<int>(id);
    }

    // Add an object using a loaded asset
    int add_object(int asset_id, py::array_t<float> transform, const std::string& name = "") {
        const float* transform_ptr = nullptr;
        float identity[16];

        if (transform.size() == 16) {
            transform_ptr = transform.data();
        } else {
            matrix::identity(identity);
            transform_ptr = identity;
        }

        ObjectId id = m_scene.add_object(
            static_cast<AssetId>(asset_id),
            transform_ptr,
            name.empty() ? nullptr : name.c_str()
        );
        return static_cast<int>(id);
    }

    // Add object directly from file path
    int add_object_from_file(const std::string& path, py::array_t<float> transform, const std::string& name = "") {
        const float* transform_ptr = nullptr;
        float identity[16];

        if (transform.size() == 16) {
            transform_ptr = transform.data();
        } else {
            matrix::identity(identity);
            transform_ptr = identity;
        }

        ObjectId id = m_scene.add_object_from_file(
            path,
            transform_ptr,
            name.empty() ? nullptr : name.c_str()
        );
        return static_cast<int>(id);
    }

    // Remove an object
    bool remove_object(int object_id) {
        return m_scene.remove_object(static_cast<ObjectId>(object_id));
    }

    // Set object transform (4x4 matrix as flat array)
    void set_transform(int object_id, py::array_t<float> transform) {
        if (transform.size() != 16) {
            throw std::runtime_error("Transform must be a 4x4 matrix (16 floats)");
        }
        m_scene.set_transform(static_cast<ObjectId>(object_id), transform.data());
    }

    // Set object position
    void set_position(int object_id, float x, float y, float z) {
        m_scene.set_position(static_cast<ObjectId>(object_id), x, y, z);
    }

    // Set object rotation (Euler angles in radians)
    void set_rotation(int object_id, float rx, float ry, float rz) {
        m_scene.set_rotation(static_cast<ObjectId>(object_id), rx, ry, rz);
    }

    // Set object scale
    void set_scale(int object_id, float scale) {
        m_scene.set_scale(static_cast<ObjectId>(object_id), scale);
    }

    void set_scale_xyz(int object_id, float sx, float sy, float sz) {
        m_scene.set_scale(static_cast<ObjectId>(object_id), sx, sy, sz);
    }

    // Set visibility
    void set_visible(int object_id, bool visible) {
        m_scene.set_visible(static_cast<ObjectId>(object_id), visible);
    }

    // Get object bounds
    py::tuple get_object_bounds(int object_id) {
        const SceneObject* obj = m_scene.get_object(static_cast<ObjectId>(object_id));
        if (!obj) {
            return py::make_tuple(
                py::make_tuple(0.0f, 0.0f, 0.0f),
                py::make_tuple(0.0f, 0.0f, 0.0f)
            );
        }
        return py::make_tuple(
            py::make_tuple(obj->bounds_min[0], obj->bounds_min[1], obj->bounds_min[2]),
            py::make_tuple(obj->bounds_max[0], obj->bounds_max[1], obj->bounds_max[2])
        );
    }

    // Get scene bounds
    py::tuple get_bounds() {
        float min[3], max[3];
        m_scene.get_bounds(min, max);
        return py::make_tuple(
            py::make_tuple(min[0], min[1], min[2]),
            py::make_tuple(max[0], max[1], max[2])
        );
    }

    // Get stats
    py::dict get_stats() {
        SceneStats stats = m_scene.get_stats();
        py::dict result;
        result["total_objects"] = stats.total_objects;
        result["visible_objects"] = stats.visible_objects;
        result["unique_assets"] = stats.unique_assets;
        result["instanced_objects"] = stats.instanced_objects;
        result["total_clusters"] = stats.total_clusters;
        result["total_meshlets"] = stats.total_meshlets;
        return result;
    }

    // Get number of objects
    size_t object_count() const {
        return m_scene.objects().size();
    }

    // Clear scene
    void clear() {
        m_scene.clear();
    }

    // Update scene (recompute bounds etc)
    void update() {
        m_scene.update();
    }

    // Get internal scene (for renderer access)
    Scene& scene() { return m_scene; }
    const Scene& scene() const { return m_scene; }

private:
    Scene m_scene;
};

// Query visible geometry for Cycles export
py::dict query_visible_geometry(PyScene& scene, PyCamera& camera, float error_threshold) {
    camera.update();

    std::vector<DrawCommand> commands;
    scene.scene().collect_draw_commands(camera.camera, error_threshold, commands);

    // Collect all visible triangles
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> indices;

    uint32_t vertex_offset = 0;

    for (const auto& cmd : commands) {
        const VGeoAsset* asset = scene.scene().get_asset(
            scene.scene().get_object(cmd.object_id)->asset_id
        );
        if (!asset) continue;

        // For each meshlet in this draw command
        for (uint32_t m = cmd.meshlet_start; m < cmd.meshlet_start + cmd.meshlet_count; m++) {
            if (m >= asset->meshlets.size()) continue;
            const auto& meshlet = asset->meshlets[m];

            // Get triangles from this meshlet
            for (uint32_t t = 0; t < meshlet.triangle_count; t++) {
                for (uint32_t v = 0; v < 3; v++) {
                    uint32_t local_idx_offset = meshlet.index_offset + t * 3 + v;
                    if (local_idx_offset >= asset->indices.size()) continue;

                    uint32_t global_idx = asset->indices[local_idx_offset];
                    if (global_idx * 3 + 2 >= asset->positions.size()) continue;

                    // Transform position by object matrix
                    float pos[3] = {
                        asset->positions[global_idx * 3 + 0],
                        asset->positions[global_idx * 3 + 1],
                        asset->positions[global_idx * 3 + 2]
                    };
                    float transformed[3];
                    matrix::transform_point(transformed, cmd.transform, pos);

                    positions.push_back(transformed[0]);
                    positions.push_back(transformed[1]);
                    positions.push_back(transformed[2]);

                    // Decode normal (octahedral)
                    if (global_idx < asset->normals.size()) {
                        float nx = static_cast<float>(asset->normals[global_idx].x) / 32767.0f;
                        float ny = static_cast<float>(asset->normals[global_idx].y) / 32767.0f;
                        float nz = 1.0f - std::abs(nx) - std::abs(ny);
                        if (nz < 0) {
                            float tx = nx;
                            nx = (1.0f - std::abs(ny)) * (nx >= 0 ? 1.0f : -1.0f);
                            ny = (1.0f - std::abs(tx)) * (ny >= 0 ? 1.0f : -1.0f);
                        }
                        float len = std::sqrt(nx*nx + ny*ny + nz*nz);
                        normals.push_back(nx / len);
                        normals.push_back(ny / len);
                        normals.push_back(nz / len);
                    } else {
                        normals.push_back(0.0f);
                        normals.push_back(1.0f);
                        normals.push_back(0.0f);
                    }

                    indices.push_back(vertex_offset++);
                }
            }
        }
    }

    py::dict result;
    result["positions"] = py::array_t<float>(positions.size(), positions.data());
    result["normals"] = py::array_t<float>(normals.size(), normals.data());
    result["indices"] = py::array_t<uint32_t>(indices.size(), indices.data());
    result["vertex_count"] = vertex_offset;
    result["triangle_count"] = vertex_offset / 3;

    return result;
}

} // namespace vgeo

PYBIND11_MODULE(vgeo_native, m) {
    m.doc() = "VGEO Native Python Bindings - Virtualized Geometry for Blender";

    // Camera
    py::class_<vgeo::PyCamera>(m, "Camera")
        .def(py::init<>())
        .def("update", &vgeo::PyCamera::update)
        .def("set_position", &vgeo::PyCamera::set_position)
        .def("set_target", &vgeo::PyCamera::set_target)
        .def_readwrite("fov", &vgeo::PyCamera::fov)
        .def_readwrite("near_plane", &vgeo::PyCamera::near_plane)
        .def_readwrite("far_plane", &vgeo::PyCamera::far_plane)
        .def_readwrite("aspect", &vgeo::PyCamera::aspect);

    // Scene
    py::class_<vgeo::PyScene>(m, "Scene")
        .def(py::init<>())
        .def("load_asset", &vgeo::PyScene::load_asset,
             py::arg("path"),
             "Load a .vgeo asset file and return its ID")
        .def("add_object", &vgeo::PyScene::add_object,
             py::arg("asset_id"),
             py::arg("transform") = py::array_t<float>(),
             py::arg("name") = "",
             "Add an object using a loaded asset")
        .def("add_object_from_file", &vgeo::PyScene::add_object_from_file,
             py::arg("path"),
             py::arg("transform") = py::array_t<float>(),
             py::arg("name") = "",
             "Load asset and add object in one call")
        .def("remove_object", &vgeo::PyScene::remove_object)
        .def("set_transform", &vgeo::PyScene::set_transform)
        .def("set_position", &vgeo::PyScene::set_position)
        .def("set_rotation", &vgeo::PyScene::set_rotation)
        .def("set_scale", &vgeo::PyScene::set_scale)
        .def("set_scale_xyz", &vgeo::PyScene::set_scale_xyz)
        .def("set_visible", &vgeo::PyScene::set_visible)
        .def("get_object_bounds", &vgeo::PyScene::get_object_bounds)
        .def("get_bounds", &vgeo::PyScene::get_bounds)
        .def("get_stats", &vgeo::PyScene::get_stats)
        .def("object_count", &vgeo::PyScene::object_count)
        .def("clear", &vgeo::PyScene::clear)
        .def("update", &vgeo::PyScene::update);

    // Geometry query for Cycles
    m.def("query_visible_geometry", &vgeo::query_visible_geometry,
          py::arg("scene"),
          py::arg("camera"),
          py::arg("error_threshold") = 1.0f,
          "Query visible geometry at given camera position for Cycles export");

    // Version info
    m.attr("__version__") = "0.1.0";
}
