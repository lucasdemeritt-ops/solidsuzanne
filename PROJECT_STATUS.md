# VGEO Project Status

**Last Updated:** January 2026
**Current Branch:** `feature/blender-viewport`

---

## What is VGEO?

A Nanite-style virtualized geometry system for Blender. The goal is to enable real-time viewport rendering of billion-polygon assets with automatic LOD selection, then hand off to Cycles for final renders.

---

## Completed Phases

### Phase 1: Preprocessing Pipeline ✅
- Meshlet generation (64 vertices, 124 triangles max)
- Cluster hierarchy with parent-child DAG
- LOD generation with simplification
- `.vgeo` binary format

### Phase 2: Runtime Viewer ✅
- Vulkan renderer with indirect drawing
- CPU frustum culling
- LOD selection based on screen-space error
- Debug visualization (cluster colors, wireframe, bounds)

### Phase 3.1: Scene Graph ✅
- Multi-object scene management
- Per-object transforms
- Asset caching for instancing
- Model rotation in viewer (arrow keys)

---

## In Progress

### Phase 3.2: Blender Render Engine
- [ ] RenderEngine subclass skeleton
- [ ] Vulkan ↔ Blender window interop
- [ ] Python bindings

---

## Quick Start

```bash
# Build
cd NewRepo
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Convert a model to .vgeo
build\tools\vgeo_build\Release\vgeo_build.exe input.glb output.vgeo

# View it
build\src\viewer\Release\vgeo_viewer.exe output.vgeo
```

**Viewer Controls:**
- Left Mouse + Drag: Orbit camera
- Right Mouse + Drag: Pan camera
- Scroll: Zoom
- Arrow Keys: Rotate model
- R: Reset model rotation
- C: Toggle cluster colors
- B: Toggle bounding boxes
- W: Toggle wireframe
- ESC: Quit

---

## Key Files

| Component | Location |
|-----------|----------|
| Core format/loader | `src/core/` |
| Preprocessing | `src/preprocess/` |
| Scene graph | `src/scene/` |
| Vulkan renderer | `src/runtime/` |
| Standalone viewer | `src/viewer/` |
| CLI tools | `tools/` |
| Blender addon | `addons/vgeo_blender/` |
| Documentation | `docs/` |

---

## Design Decisions

- **Blender version:** 4.x
- **VGEO meshes:** Read-only (re-import to update)
- **Instancing:** Supported (same asset, different transforms)
- **Materials:** Full textures planned for viewport + Cycles

---

## Git History

Recent commits on `feature/blender-viewport`:
- `678a10a` - Add scene graph and model rotation support
- `9401202` - Add Phase 3 spec: Blender Viewport Integration
- `151ad72` - Fix index remapping in vgeo writer
- `f4b8901` - Fix viewer hang by loading shaders from files
