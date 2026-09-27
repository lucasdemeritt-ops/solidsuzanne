# VGEO web viewer

`vgeo-viewer.js` streams a cluster-LOD asset with WebGPU. One ES module, no
build step. Compressed `.vgeow` files also need `meshopt_decoder.mjs` (MIT,
from meshoptimizer) next to it; it is loaded only when needed.

In Blender, **VGEO > Export for Web** writes a ready-to-host folder: the
`.vgeow` asset, both scripts and an `index.html`.

```html
<canvas id="c" style="width:100%;height:100vh"></canvas>
<script type="module">
  import { createViewer } from "./vgeo-viewer.js";
  const viewer = await createViewer(document.getElementById("c"), "asset.vgeo", { pixelError: 1 });
</script>
```

`index.html` is a full demo: stats, pixel-error slider, Shaded / LOD / Clusters
views, and drag-and-drop of any `.vgeo` file. Serve the folder over HTTP
(`python -m http.server`) and open `index.html?src=assets/your.vgeo`.

## How it works

Every frame one compute pass tests every cluster against the camera with the
same rule the Blender add-on uses (a cluster is drawn when its group is too
coarse for the view and the group it came from is fine enough), culls it
against the frustum and appends it to a visible list. One indirect instanced
draw then renders all visible clusters, pulling vertices from storage
buffers. The CPU never touches geometry after upload.

The GPU cut is checked against the native runtime: `tests/web/compare.html`
renders fixed views with culling off, `tests/web/native_compare.py` selects
the same views natively, and triangle and cluster counts must match exactly.

Materials: base color and roughness per material are embedded in the file by
the Blender add-on; `options.materials` overrides them (array, or object keyed
by material name).

## Formats

| | `.vgeo` (v2) | `.vgeow` (web) |
|---|---|---|
| Written by | Virtualize | Export for Web |
| Vertices | shared, float32 | per cluster, 21-bit global grid, oct normals |
| Streams | raw | meshopt-compressed |
| 2.1M-triangle terrain | 75 MB (32 MB gzipped) | 33 MB |

Both decode to the same GPU layout. Positions sit on one global grid, so a
vertex shared by two clusters quantizes identically in both and cuts stay
watertight (`tests/test_web_layout.mjs` welds decoded cuts and counts open
edges). The in-browser conversion of `.vgeo` is bit-identical to the native
exporter.

## Limits today

- The whole file is downloaded and uploaded to the GPU; fine for assets up to a
  few million triangles. Streaming clusters by page (HTTP range requests) is
  the next step, and the per-cluster layout was chosen for it.
- Needs WebGPU (current Chrome, Edge, Safari 26+, Firefox 141+ on Windows).
- Simple sun + sky lighting; no textures yet.
