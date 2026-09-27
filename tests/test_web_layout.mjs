// node tests/test_web_layout.mjs <closed_asset.vgeo> <same_asset.vgeow>
// Decodes both formats into the viewer's GPU layout and checks that cuts stay
// watertight after quantization: vertices are welded by their grid values
// (exactly what the GPU draws) and open edges are counted.
import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { parseVGEO, toGPULayout } from "../web/vgeo-viewer.js";

const decoder = pathToFileURL(new URL("../web/meshopt_decoder.mjs", import.meta.url).pathname.replace(/^\/(\w:)/, "$1"));
let failed = 0;
const check = (name, ok, detail = "") => { console.log((ok ? "PASS " : "FAIL ") + name + (detail ? `  [${detail}]` : "")); if (!ok) failed++; };

function load(file) {
  const b = readFileSync(file);
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
}

// the viewer's selection rule, on the CPU
function cut(layout, cam, proj, threshold) {
  const g = new Float32Array(layout.groups.buffer, layout.groups.byteOffset, layout.groups.length);
  const passes = (i) => {
    const e = g[i * 8 + 4];
    if (e > 1e37) return false;
    const d = Math.hypot(g[i * 8] - cam[0], g[i * 8 + 1] - cam[1], g[i * 8 + 2] - cam[2]) - g[i * 8 + 3];
    return e / Math.max(d, 1e-4) * proj * 0.5 <= threshold;
  };
  const out = [];
  const c = layout.clusters;
  for (let i = 0; i < c.length / 12; i++) {
    const refined = c[i * 12 + 3] | 0;
    if (!passes(c[i * 12 + 2]) && (refined < 0 || passes(refined))) out.push(i);
  }
  return out;
}

function openEdges(layout, clusterIds) {
  const key = new Map();
  const vid = (v) => {
    const k = `${layout.vertices[v * 4]},${layout.vertices[v * 4 + 1]},${layout.vertices[v * 4 + 2]}`;
    let id = key.get(k);
    if (id === undefined) { id = key.size; key.set(k, id); }
    return id;
  };
  const edges = new Map();
  let tris = 0;
  for (const cid of clusterIds) {
    const vo = layout.clusters[cid * 12], to = layout.clusters[cid * 12 + 1], tc = layout.clusters[cid * 12 + 4] >>> 16;
    for (let t = 0; t < tc; t++) {
      const w = layout.triangles[to + t];
      const ids = [vid(vo + (w & 255)), vid(vo + ((w >>> 8) & 255)), vid(vo + ((w >>> 16) & 255))];
      for (let j = 0; j < 3; j++) {
        const a = ids[j], b = ids[(j + 1) % 3];
        const k = a < b ? a * 4294967296 + b : b * 4294967296 + a;
        edges.set(k, (edges.get(k) || 0) + 1);
      }
      tris++;
    }
  }
  // open meshes (terrain) have a real border: ignore edges lying on the bounding box sides
  const coords = [...key.keys()].map((k) => k.split(",").map(Number));
  const { lo, hi } = gridBounds(layout);
  const onSide = (id) => coords[id].some((v, a) => a < 2 && (v === lo[a] || v === hi[a]));
  let open = 0;
  for (const [k, n] of edges) {
    if (n !== 1) continue;
    const a = Math.floor(k / 4294967296), b = k % 4294967296;
    if (IGNORE_BORDER && onSide(a) && onSide(b)) continue;
    open++;
  }
  return { open, tris };
}
const IGNORE_BORDER = process.argv.includes("--open-mesh");
const _bounds = new WeakMap();
function gridBounds(layout) {
  let b = _bounds.get(layout);
  if (!b) {
    b = { lo: [Infinity, Infinity, Infinity], hi: [-Infinity, -Infinity, -Infinity] };
    for (let i = 0; i < layout.vertices.length; i += 4) {
      for (let a = 0; a < 3; a++) {
        const v = layout.vertices[i + a];
        if (v < b.lo[a]) b.lo[a] = v;
        if (v > b.hi[a]) b.hi[a] = v;
      }
    }
    _bounds.set(layout, b);
  }
  return b;
}

const [v2file, webfile] = process.argv.slice(2).filter((a) => !a.startsWith("--"));
const v2buf = load(v2file), webbuf = load(webfile);
const h2 = parseVGEO(v2buf), hw = parseVGEO(webbuf);
check("formats detected", h2.format === "vgeo" && hw.format === "vgeow" && (hw.flags & 1) === 1);
const L2 = await toGPULayout(v2buf, h2);
const LW = await toGPULayout(webbuf, hw, decoder);
check("same cluster count", L2.clusters.length === LW.clusters.length, String(LW.clusters.length / 12));
check("decoded streams identical to in-browser conversion",
  L2.vertices.length === LW.vertices.length && L2.triangles.every((x, i) => x === LW.triangles[i]) &&
  L2.vertices.every((x, i) => x === LW.vertices[i]), `${LW.vertices.length / 4} vertices`);
check("compressed file is smaller", webbuf.byteLength < v2buf.byteLength,
  `${(v2buf.byteLength / 2 ** 20).toFixed(1)} -> ${(webbuf.byteLength / 2 ** 20).toFixed(1)} MB`);

const full = [];
for (let i = 0; i < LW.clusters.length / 12; i++) if (LW.clusters[i * 12 + 5] === 0) full.push(i);
let r = openEdges(LW, full);
check("full detail watertight after quantization", r.open === 0, `${r.open} open edges, ${r.tris} tris`);
// mixed-LOD cuts from a few viewpoints
const lo = hw.aabbMin, hi = hw.aabbMax;
const size = Math.hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
for (const [cam, px] of [[[0, -size * 0.9, 0], 1], [[size * 0.7, 0, size * 0.3], 2], [[0, 0, -size * 3], 4]]) {
  const ids = cut(LW, cam, 2.414, px / 600);
  const depths = new Set(ids.map((i) => LW.clusters[i * 12 + 5]));
  r = openEdges(LW, ids);
  check(`mixed cut watertight (${depths.size} LOD levels)`, r.open === 0, `${r.open} open edges, ${r.tris} tris`);
}
console.log(`\n${failed ? "FAILED " + failed : "all passed"}`);
process.exit(failed ? 1 : 0);
