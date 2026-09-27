# Native streaming: virtualized geometry rendered by EEVEE and Cycles

The original VGEO path renders meshlets with its own headless Vulkan
renderer and copies the pixels into the viewport. That can never look like
EEVEE or reach Cycles, because it replaces Blender's renderers instead of
feeding them.

The native streaming path keeps Blender's renderers and virtualizes the
*geometry* instead: a Nanite-style cluster DAG lives on disk and in the
native library, and only the view-dependent cut is handed to Blender as
ordinary meshes. Everything Blender can do with a mesh (materials, lights,
shadows, ray tracing, Cycles, EEVEE) just works, and Blender only ever holds
the triangles the current view needs.

## Pipeline

```
Blender mesh (modifiers applied)
    │  addons/vgeo/build.py: triangles, normals, UVs, materials (indexed when smooth)
    ▼
vgeo_stream.dll: vgeo_build
    │  weld → meshoptimizer clusterlod (group clusters, simplify each group with
    │  its border locked, re-split, repeat) → spatial chunks → .vgeo v2
    ▼
.vgeo file (next to the .blend, in //vgeo/)
    │
    ▼
vgeo_stream.dll: vgeo_select / vgeo_extract          (ctypes, no Python ABI)
    │  per view: pick the crack-free cut, hash each chunk's cluster set,
    │  extract changed chunks as indexed meshes with edges
    ▼
addons/vgeo/stream.py
    │  proxy object → Geometry Nodes → Collection Info → chunk objects
    │  live loop in the viewport, render_pre handler for final renders
    ▼
EEVEE / Cycles / Workbench
```

## Why the cut never cracks

Each cluster in group `g`, simplified from group `r`, is drawn when
`error(g) > t` and `error(r) <= t` (or it is original geometry). Group bounds
enclose their children's bounds and errors only grow up the DAG, so the
decision is monotonic and neighbouring clusters always agree on the border
they share. The tests weld every cut and count open edges: always zero.

**Off-screen coarsening** (the default) raises the threshold for groups
entirely outside the view. That stays crack-free: a group outside the view
has all its finer groups outside too, so monotonicity holds. Off-screen
geometry is kept at low detail, so it still casts shadows and shows in
reflections.

## Landing updates without hitches

Every chunk is a pair of objects. How an update lands depends on the views:

| Views | Strategy | Why |
|---|---|---|
| Solid / Workbench | **staged**: fill the hidden back (scale 0, no shadows) a few ms per tick, then flip scales | Blender prepares the back's GPU buffers as it fills; the flip is a transform change |
| EEVEE (Material Preview, Rendered) | **batch**: fill unlinked spare meshes, land them all in one frame | EEVEE pays heavily for every frame in which geometry changes |
| Cycles Rendered | **settle**: update once the view is still for 0.3 s | Cycles rebuilds its BVH and restarts sampling anyway |

Lessons measured along the way (tests/viewport_bench.py, tests/swap_bench.py):

- Creating or deleting datablocks, and assigning materials, force a
  depsgraph relations rebuild; mid-stream material assignment made EEVEE
  re-sync the whole scene every frame. Streaming now creates nothing and
  assigns materials up front.
- `foreach_set` takes a per-item path for topology arrays. The add-on copies
  arrays straight into mesh attributes (verified once per session on a fixed
  mesh, falling back to `foreach_set`), and the native library computes
  edges so `mesh.update()` does not. Writes went from 0.9M to 8-10M
  triangles/s.
- Around 512 chunks is the sweet spot: 2048 objects made every frame and
  every relations rebuild slower than the smaller updates saved.

## Measured (RTX A4500 over a remote desktop, Blender 5.1.2)

Terrain demo: 33.5M triangles from Geometry Nodes (`examples/terrain_demo.py`).

| | |
|---|---|
| Build (clusterlod, single thread) | 129-141 s, 19 LOD levels, 512 chunks, 1.2 GB file |
| Cut from the hero camera, 1080p, 1 px, off-screen coarsened | 2.5M triangles (7.4 %) |
| Full rebuild of a 7.7M-triangle cut | 1.3 s |
| Viewport idle, Solid / EEVEE | ~75 / ~55 fps |
| Viewport while continuously streaming, Solid | ~28 fps, worst frame ~50 ms |
| Viewport while continuously streaming, EEVEE | ~29 fps, one ~0.4 s frame when a large update lands |
| Cycles, 1080p, 64 samples, same material: raw 33.5M mesh | 93-118 s |
| Cycles, same frame from the VGEO cut (9.8M triangles, 0.5 px) | 49-51 s |
| Difference between the two images | mean 0.02/255, 99th percentile 1/255 |

## Instancing (full scenes)

`Scatter Instances` (active VGEO object + another selected mesh) creates an
instancer: a point mesh whose vertices are placements with rotation and scale
attributes. The asset's uniform LOD levels are built once into shared level
meshes (each a complete, crack-free cut); every update picks, per placement,
the coarsest level whose error stays under the pixel threshold, and writes it
to an integer attribute that a Geometry Nodes Instance on Points tree reads.
Navigating never rebuilds geometry, and EEVEE and Cycles get real instances.

Scatter demo (`examples/scatter_demo.py`): 2,000 copies of a 1.3M-triangle
boulder = 2.6 billion source triangles.

| | |
|---|---|
| Build | virtualize 3.6 s, 15 level meshes 0.3 s |
| Viewport cut at 1 px | 6.6M triangles (0.25 %) |
| Final render at 0.5 px, 1600x900 | 19.4M triangles: EEVEE 1.7 s, Cycles 8.3 s (64 samples, OptiX) |
| Viewport while flying | Solid ~117 fps, EEVEE ~68 fps, worst frame 45 ms |

## On the web

`web/vgeo-viewer.js` renders the same assets with WebGPU: a compute pass
applies the same cut rule per cluster and one indirect draw renders the
result; its cut matches the native runtime to the triangle. **Export for Web**
writes a compact `.vgeow` (meshopt-compressed, positions on a global 21-bit
grid so cuts stay watertight) with the viewer and a page. See `web/README.md`.

## Using it

1. Build `vgeo_stream` (see below); the DLL lands in `addons/vgeo/bin/`.
2. Install `addons/vgeo` as an add-on (or zip it with `tools/package_addon.py`).
3. Select a mesh, **Object > Virtualize Mesh** (or the VGEO sidebar tab).
   The original is hidden and kept (or removed, if you ask).
4. The proxy streams automatically. Sidebar settings: viewport and render
   pixel error, off-screen mode, freeze, LOD colors, restore.

Save the .blend first so the asset lands in `//vgeo/` next to it.

## Building

```
cmake -S . -B build-stream -G Ninja -DCMAKE_BUILD_TYPE=Release -DVGEO_STREAM_ONLY=ON
cmake --build build-stream
```

Needs a C++20 compiler; no Vulkan SDK, no Python headers. meshoptimizer is
fetched at a pinned commit (or pass `-DVGEO_MESHOPTIMIZER_DIR=...`).
The DLL links the C runtime statically so it loads inside Blender with no
redistributable installed.

## Tests

```
blender -b --factory-startup --python tests/test_stream.py
blender scene.blend --python tests/viewport_bench.py -- --motion fly|orbit
```

## Toward the web

The .vgeo v2 layout (separate vertex arrays, clusters, groups, chunks) was
chosen so a WebGPU runtime can read the same file: the selection rule is the
same few lines, and `vgeo_stream` can be compiled to WebAssembly for the
cut selection if needed.

## Not yet

- Instances use whole-asset levels; a placement close enough to need a
  streamed cut of its own (very large assets filling the view) is next.
- Building without a full Blender mesh in memory (import straight from
  disk, or tiled builds), for sources beyond what Blender can hold.
- Streaming clusters from disk (the file is read whole today).
- Rare back-to-back "fins" left by simplification (about 3 per 100k
  triangles at coarse levels) are harmless but could be filtered.
