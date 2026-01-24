#pragma once

#include "mesh_import.h"
#include "meshlet_gen.h"
#include "hierarchy.h"
#include <string>

namespace vgeo {

// Write parameters
struct WriteParams {
    bool compress = false;
    bool quantize_positions = false;
    uint8_t position_bits = 16;  // 16 or 21
};

// Write .vgeo file
bool write_vgeo(
    const std::string& path,
    const RawMesh& mesh,
    const MeshletData& meshlets,
    const HierarchyData& hierarchy,
    const WriteParams& params = {}
);

} // namespace vgeo
