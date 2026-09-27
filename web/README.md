# VGEO web viewer

`vgeo-viewer.js` streams a `.vgeo` cluster-LOD asset with WebGPU. One ES
module, no build step, no dependencies.

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

## Limits today

- The whole file is downloaded and uploaded to the GPU; fine for assets up to a
  few million triangles (a 2M-triangle asset is 79 MB). Quantized, compressed
  and streamed-by-page files are the next step.
- Needs WebGPU (current Chrome, Edge, Safari 26+, Firefox 141+ on Windows).
- Simple sun + sky lighting; no textures yet.
