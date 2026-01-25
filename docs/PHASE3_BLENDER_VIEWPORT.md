# Phase 3: Blender Viewport Integration

## Vision

Transform VGEO from a standalone viewer into a **Blender viewport render engine** that enables real-time manipulation of virtualized geometry, with seamless handoff to Cycles for final renders.

**Goal:** Work with billion-polygon assets in Blender's viewport at interactive framerates, just like Nanite in Unreal.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                         BLENDER                                 │
├────────────────────────────┬────────────────────────────────────┤
│   VGEO Viewport Engine     │      Cycles Integration            │
│   (Real-time editing)      │      (Final renders)               │
│                            │                                    │
│   ┌──────────────────┐     │     ┌────────────────────────┐     │
│   │ Vulkan Renderer  │     │     │ Geometry Query API     │     │
│   │ - GPU LOD select │     │     │ - Get visible tris     │     │
│   │ - Frustum cull   │     │     │ - Camera-based LOD     │     │
│   │ - HZB occlusion  │     │     │ - Export to Cycles     │     │
│   └──────────────────┘     │     └────────────────────────┘     │
│            │               │                │                   │
├────────────┴───────────────┴────────────────┴───────────────────┤
│                      VGEO Core Library                          │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │ Scene Graph │  │ Asset Cache │  │ Hierarchy Traversal     │  │
│  │ - Objects   │  │ - .vgeo     │  │ - Visibility queries    │  │
│  │ - Transforms│  │ - LOD data  │  │ - LOD selection         │  │
│  └─────────────┘  └─────────────┘  └─────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Key Components

### 1. VGEO Render Engine (Python + C++)

A custom `bpy.types.RenderEngine` subclass that:
- Renders the viewport using our Vulkan pipeline
- Draws to offscreen buffer, blits to Blender's viewport
- Handles view updates, object transforms, camera changes

```python
class VGEORenderEngine(bpy.types.RenderEngine):
    bl_idname = "VGEO"
    bl_label = "VGEO Viewport"
    bl_use_preview = True

    def view_update(self, context, depsgraph):
        # Scene changed - update transforms, add/remove objects

    def view_draw(self, context, depsgraph):
        # Draw frame using VGEO renderer
```

### 2. Scene Graph

Manage multiple VGEO objects with transforms:

```cpp
struct VGeoObject {
    uint32_t asset_id;           // Reference to loaded .vgeo
    float transform[16];         // Model matrix
    float bounds_world[6];       // Transformed AABB for culling
    bool visible;
    bool selected;
};

class VGeoScene {
    std::vector<VGeoObject> objects;
    std::unordered_map<uint32_t, VGeoAsset*> asset_cache;

    void add_object(const std::string& vgeo_path, const float* transform);
    void update_transform(uint32_t object_id, const float* transform);
    void remove_object(uint32_t object_id);

    // For rendering
    void cull_and_collect(const Camera& camera,
                          std::vector<DrawCommand>& out_commands);
};
```

### 3. Blender ↔ C++ Bridge

Python bindings via pybind11 or ctypes:

```python
# Python side
import vgeo_native

# Initialize renderer with Blender's window
vgeo_native.init(window_handle, width, height)

# Add VGEO object to scene
obj_id = vgeo_native.add_object("mesh.vgeo", matrix_world)

# Update transform when object moves
vgeo_native.update_transform(obj_id, matrix_world)

# Render frame
vgeo_native.render(view_matrix, projection_matrix)

# Get pixels for Blender
pixels = vgeo_native.get_framebuffer()
```

### 4. Cycles Bridge

For final renders, extract visible geometry:

```cpp
// Query visible triangles at given camera
struct TriangleOutput {
    std::vector<float> positions;  // x,y,z per vertex
    std::vector<float> normals;
    std::vector<uint32_t> indices;
};

TriangleOutput query_visible_geometry(
    const VGeoScene& scene,
    const Camera& camera,
    float error_threshold  // Controls LOD level
);
```

This can be called from Python and passed to Cycles as mesh data.

---

## Implementation Phases

### Phase 3.1: Scene Graph & Multi-Object Support
- [ ] VGeoScene class with object management
- [ ] Per-object transforms (model matrices)
- [ ] Batch rendering of multiple objects
- [ ] Object picking/selection support

### Phase 3.2: Blender Render Engine
- [ ] RenderEngine subclass skeleton
- [ ] Window/context sharing with Blender
- [ ] Offscreen rendering + blit to viewport
- [ ] View matrix sync from Blender camera
- [ ] Handle viewport resize

### Phase 3.3: Object Integration
- [ ] Custom object type or mesh proxy for VGEO assets
- [ ] Import operator (OBJ/glTF → VGEO → Blender object)
- [ ] Transform sync (Blender object ↔ VGEO scene)
- [ ] Outliner integration

### Phase 3.4: Cycles Bridge
- [ ] Geometry query API (get triangles at LOD)
- [ ] Export visible geometry to Cycles mesh
- [ ] Material passthrough (basic)
- [ ] Animation support (per-frame LOD query)

### Phase 3.5: Polish & Performance
- [ ] GPU-driven culling (move from CPU)
- [ ] Streaming for huge scenes
- [ ] Instancing support
- [ ] Debug overlays (show LOD levels, cluster bounds)

---

## Technical Challenges

### 1. Vulkan ↔ Blender Window
- Blender uses OpenGL (EEVEE) or its own GPU abstraction
- Options:
  - Render to offscreen Vulkan buffer → copy to OpenGL texture → blit
  - Use Blender's `gpu` module with custom shaders (limited)
  - External window overlay (less integrated)

**Recommended:** Offscreen Vulkan → OpenGL interop or CPU readback → blit

### 2. Object Identity
- Blender objects have their own ID system
- Need mapping: Blender Object ↔ VGEO Object ID
- Handle object deletion, duplication, etc.

### 3. Material System
- VGEO currently has no materials
- For viewport: simple procedural materials or vertex colors
- For Cycles: pass through Blender materials

### 4. Animation
- Object transforms animate via Blender's animation system
- Per-frame: sync transforms → render
- LOD recalculated each frame (fast)

---

## File Structure

```
NewRepo/
├── src/
│   ├── core/           # Shared (loader, formats)
│   ├── scene/          # NEW: Scene graph
│   │   ├── scene.h
│   │   ├── scene.cpp
│   │   └── object.h
│   ├── runtime/        # Vulkan renderer (extended)
│   └── blender/        # NEW: Blender integration
│       ├── engine.py       # RenderEngine
│       ├── operators.py    # Import/export
│       ├── panels.py       # UI
│       └── native/         # C++ bindings
│           ├── bindings.cpp
│           └── CMakeLists.txt
├── addons/
│   └── vgeo_blender/   # Existing addon (extend)
```

---

## Success Criteria

1. **Import .vgeo into Blender scene** as manipulable object
2. **Real-time viewport** at 60+ FPS with million-poly assets
3. **Object transforms** update in real-time (move, rotate, scale)
4. **Multiple objects** in scene with independent LOD
5. **Cycles render** produces correct output at appropriate LOD
6. **No per-frame reprocessing** - only LOD queries

---

## Open Questions

1. **Blender version target?** 3.6 LTS? 4.x? (GPU API differs)
2. **Material support scope?** Viewport only? Cycles passthrough?
3. **Instancing?** Same .vgeo with different transforms?
4. **Edit mode?** Can you edit VGEO meshes or are they read-only?

---

## Next Steps

1. Review and refine this spec
2. Prototype RenderEngine in Python (minimal)
3. Test Vulkan ↔ Blender window interop
4. Build scene graph in C++
5. Create Python bindings
