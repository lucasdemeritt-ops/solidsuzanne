// VGEO glTF 2.0 Importer
// Load .gltf and .glb files

#pragma once

#include "mesh_import.h"
#include <string>

namespace vgeo {

// Load a glTF 2.0 file (.gltf or .glb)
// Extracts mesh geometry (positions, normals, UVs, indices)
// Combines all mesh primitives into a single RawMesh
bool load_gltf(const std::string& path, RawMesh& out_mesh);

} // namespace vgeo
