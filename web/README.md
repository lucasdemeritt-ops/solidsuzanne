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

| | `.vgeo` (v2) | `.vgeow` v1 | `.vgeow` v2 (paged) |
|---|---|---|---|
| Written by | Virtualize | `build_web_asset()` (default) | Export for Web, `export_web(paged=True)` |
| Vertices | shared, float32 | per cluster, 21-bit global grid, oct normals | same as v1 |
| Streams | raw | one meshopt stream each | meshopt per page |
| Loading | whole file | whole file | head + root pages, then pages on demand |
| 2.1M-triangle terrain | 75 MB (32 MB gzipped) | 33 MB | 33 MB |

All decode to the same GPU layout. Positions sit on one global grid, so a
vertex shared by two clusters quantizes identically in both and cuts stay
watertight (`tests/test_web_layout.mjs` welds decoded cuts and counts open
edges). The in-browser conversion of `.vgeo` is bit-identical to the native
exporter.

## Streaming pages (HTTP range requests)

A paged `.vgeow` holds a *head* (header, cluster and group tables, page
table, materials) and then pages. Each page holds whole groups (a group's
clusters load together), groups are ordered parents first (terminal groups,
then deepest to finest, along a Morton curve within a level), and the
leading *root pages* hold every terminal group. The cluster and group tables
are encoded with the same lossless meshopt codec as the pages (header flag
bit 1, encoded sizes at bytes 128 and 132), which makes the head about 3.7x
smaller; files written before that (flag bit 1 clear) still load.

The viewer fetches the head and the root pages with range requests and draws
at once: that is already a complete coarse model. Every few frames it
compares the views with the group errors and requests the pages of groups
the cut wants to refine into, most visible first, merging neighbouring pages
into one request (4 in flight, `maxRequests`). The GPU cut treats a group
whose page is missing as fine enough, so the coarser clusters made from it
are drawn instead. A group only counts as resident once its page and all its
parents' pages are, so any arrival order gives a valid, crack-free cut
(`tests/test_web_paged.mjs` loads pages in random orders and welds every
cut). Once the pages a view needs have arrived, the cut is exactly the v1
file's.

Terrain (2.1M triangles, 33 MB), 960x600 canvas (`tests/web/paged_check.mjs`):

| | |
|---|---|
| Before the first frame | 0.62 MB (head 0.44 MB + root page), ~0.26 s locally (1.8 MB before the head was encoded) |
| Wide view, settled | 84 of 586 pages |
| Close-up, settled | 193 pages, 11.2 MB of 31.6 MB (56 range requests) |
| Settled cuts vs the v1 file | identical triangle and cluster counts |

Servers without range support (e.g. `python -m http.server`) answer with the
whole file, and the viewer simply uses it. `tests/web/range_server.mjs` is a
small static server with ranges for local testing. `stats` reports
`bytesLoaded`, `pagesLoaded` and `pagesTotal`.

**Streamed copies.** In a scene with many placements of one asset, each copy
draws a whole-asset level; the copies close enough to want level 0 (up to
`streamedCopies`, default 4, for assets whose level 0 has 20k triangles or
more) run the per-cluster GPU cut instead, like a single placement, in the
same frame. Seen from 2.2 radii, the nearest of 400 boulders draws 682k
triangles for the scene instead of 921k.

## Limits today

- Needs WebGPU (current Chrome, Edge, Safari 26+, Firefox 141+ on Windows).
- GPU buffers are allocated at full size up front; paging saves download,
  not GPU memory.
- Simple sun + sky lighting; no textures yet.
