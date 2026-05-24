# VGEO Project Status

Single source of truth for project progress. The [README](README.md) covers
what VGEO is and how to use it; this file tracks what works, what's verified,
and what's next.

**Last updated:** 2026-05-24

---

## At a glance

| Area | State | Verified by |
|------|-------|-------------|
| Mesh import (OBJ, glTF 2.0) | Working | Unit tests |
| Meshlet generation | Working | Unit tests |
| Multi-level cluster hierarchy (DAG) | Working | Unit tests |
| QEM mesh simplification | Working (CPU module) | Unit tests |
| `.vgeo` binary format (write/load/validate) | Working | Unit tests |
| CLI tools (`vgeo_build`, `vgeo_validate`) | Working | Manual + CI build |
| Headless Vulkan renderer | Proof-of-concept | Manual (needs GPU) |
| Blender RenderEngine + addon | Proof-of-concept | Manual (needs GPU) |
| Continuous integration | Builds + tests CPU pipeline | GitHub Actions |

"Verified by unit tests" means it runs green in CI on every push. The renderer
and Blender integration require a Vulkan GPU and are exercised manually, not in
CI.

---

## The pipeline

```
OBJ / glTF  ──>  [import]  ──>  RawMesh
                                   │
                  (optional) [QEM simplify]  ──>  coarse RawMesh
                                   │
                              [meshlet gen]  ──>  meshlets + bounds + cones
                                   │
                            [build hierarchy]  ──>  multi-level cluster DAG
                                   │
                              [write .vgeo]  ──>  compact binary asset
                                   │
                     [Vulkan renderer / Blender RenderEngine]
```

Everything above the renderer is plain C++ with no GPU dependency, builds on
Linux/macOS/Windows, and is covered by the test suite.

---

## Verified preprocessing features

- OBJ import with vertex deduplication, polygon triangulation, auto normals
- glTF 2.0 import (`.gltf`/`.glb`, embedded base64 and external buffers)
- Greedy meshlet generation (configurable vertex/triangle limits)
- Bounding spheres and normal cones per meshlet
- Multi-level LOD hierarchy with spatial grouping and contiguous child ranges
- Quadric error metric (QEM) edge-collapse simplification with boundary
  preservation and normal-flip rejection
- Octahedral (snorm16) normal encoding, half-float UVs
- `.vgeo` writer / loader / validator with chunked layout

## Renderer & Blender (manual, GPU required)

These are demonstrated by the proof-of-concept in the README (live meshlet
color debug view in the Blender viewport) but are not part of automated testing:

- Headless Vulkan offscreen renderer with pixel readback
- GPU frustum culling compute pipeline; HZB pyramid generation
- pybind11 bindings exposing Scene / Camera / Renderer to Python
- `VGEORenderEngine` Blender subclass, N-panel, mesh → `.vgeo` conversion

---

## Testing

`tests/unit/test_pipeline.cpp` covers the full CPU pipeline (15 tests):
OBJ + glTF loading, meshlet generation, multi-level hierarchy integrity,
`.vgeo` roundtrip, octahedral-normal roundtrip accuracy, bounding spheres,
and QEM simplification (reduction, target count, error monotonicity,
watertight validity, empty-input rejection).

```bash
cmake -B build -DVGEO_BUILD_VIEWER=OFF -DVGEO_BUILD_PYTHON=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

CI (`.github/workflows/ci.yml`) runs this on every push and pull request.

---

## Next steps

1. Wire QEM simplification into the LOD hierarchy so internal clusters carry
   decimated geometry (currently the hierarchy stores a radius-based error
   proxy and the renderer draws leaf clusters only). The simplifier already
   returns a geometric error suitable for the cluster error metric.
2. Integrate HZB occlusion culling into the render loop (shader exists).
3. Per-cluster-group simplification for true Nanite-style continuous LOD.
4. Validate the renderer and Blender addon on Linux/macOS (currently
   Windows-focused; MoltenVK untested).
5. Performance profiling on large meshes.

---

## Build dependencies

- CMake 3.20+, C++20 compiler (CPU pipeline + tests; no GPU needed)
- Vulkan SDK 1.2+ and GLFW (renderer/viewer only; GLFW fetched via CMake)
- pybind11 (Python bindings for the Blender addon)
- Blender 4.0+ (addon)
