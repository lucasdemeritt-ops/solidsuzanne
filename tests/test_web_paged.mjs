// node tests/test_web_paged.mjs <closed_asset.vgeo> <asset.vgeow (v1)> <asset_paged.vgeow (v2)>
// Paged .vgeow: the same geometry as v1, a head that parses on its own, and cuts that stay
// watertight while pages arrive in any order (a missing group's coarser stand-ins are drawn).
import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { parseVGEO, toGPULayout, decodePage, Residency } from "../web/vgeo-viewer.js";

const decoder = pathToFileURL(new URL("../web/meshopt_decoder.mjs", import.meta.url).pathname.replace(/^\/(\w:)/, "$1"));
let failed = 0;
const check = (name, ok, detail = "") => { console.log((ok ? "PASS " : "FAIL ") + name + (detail ? `  [${detail}]` : "")); if (!ok) failed++; };
const load = (f) => { const b = readFileSync(f); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); };

const [v2file, v1file, pagedFile] = process.argv.slice(2);
const v1buf = load(v1file), pbuf = load(pagedFile);
const h1 = parseVGEO(v1buf), hp = parseVGEO(pbuf);
check("paged header", hp.version === 2 && hp.paged && !hp.partial && hp.pageCount > 1 && hp.rootPages >= 1,
  `${hp.pageCount} pages, ${hp.rootPages} root, head ${(hp.headBytes / 1024).toFixed(0)} KB of ${(hp.fileSize / 1024).toFixed(0)} KB`
  + (hp.flags & 2 ? `, encoded tables ${((hp.clusterBytes + hp.groupBytes) / 1024).toFixed(0)} KB` : ", tables not encoded (older file)"));
const L1 = await toGPULayout(v1buf, h1, decoder);
const LP = await toGPULayout(pbuf, hp, decoder);
check("same counts as v1", LP.clusters.length === L1.clusters.length && LP.vertices.length === L1.vertices.length
  && LP.triangles.length === L1.triangles.length, `${LP.clusters.length / 12} clusters`);

// every cluster carries the same geometry (clusters are reordered and groups renumbered)
const sig = (L, i) => {
  const c = L.clusters, b = i * 12, vo = c[b], to = c[b + 1], vn = c[b + 4] & 0xFFFF, tn = c[b + 4] >>> 16;
  let h = `${c[b + 5]}|${c[b + 6]},${c[b + 7]},${c[b + 8]},${c[b + 9]}|`;
  for (let k = 0; k < vn * 4; k++) h += L.vertices[vo * 4 + k].toString(36) + ",";
  for (let k = 0; k < tn; k++) h += L.triangles[to + k].toString(36) + ",";
  return h;
};
const s1 = new Map();
for (let i = 0; i < L1.clusters.length / 12; i++) { const k = sig(L1, i); s1.set(k, (s1.get(k) || 0) + 1); }
let same = true;
for (let i = 0; i < LP.clusters.length / 12 && same; i++) {
  const k = sig(LP, i), n = s1.get(k);
  if (!n) same = false; else s1.set(k, n - 1);
}
check("every cluster identical to v1", same);
// groups renumbered consistently: a cluster's group and refined group keep their errors
const g1 = new Float32Array(L1.groups.buffer), gp = new Float32Array(LP.groups.buffer);
const errOf = (L, g, i) => { const r = L.clusters[i * 12 + 3] | 0; return [g[L.clusters[i * 12 + 2] * 8 + 4], r < 0 ? -1 : g[r * 8 + 4]]; };
const e1 = new Map();
for (let i = 0; i < L1.clusters.length / 12; i++) e1.set(sig(L1, i), errOf(L1, g1, i).join());
let ge = true;
for (let i = 0; i < LP.clusters.length / 12 && ge; i++) ge = e1.get(sig(LP, i)) === errOf(LP, gp, i).join();
check("group references renumbered consistently", ge);

// the head alone parses (what the viewer fetches first)
const head = pbuf.slice(0, hp.headBytes);
const hh = parseVGEO(head);
check("head parses on its own", hh.partial && hh.pageCount === hp.pageCount && hh.materialNames.join() === hp.materialNames.join());
const LH = await toGPULayout(head, hh, decoder);
check("head layout has tables, no streams", LH.vertices === null && LH.clusters.length === LP.clusters.length);
// a page decoded from a range buffer matches the full decode
const p3 = hp.pages[Math.min(3, hp.pageCount - 1)];
const range = pbuf.slice(p3.offset, p3.offset + p3.vbytes + p3.tbytes);
const d3 = await decodePage(range, p3, hp, decoder, p3.offset);
check("page decodes from a range", d3.vertices.every((x, k) => x === LP.vertices[p3.v0 * 4 + k])
  && d3.triangles.every((x, k) => x === LP.triangles[p3.t0 + k]), `${p3.vn} vertices, ${p3.tn} triangles`);
// pages are ordered parents first: every group's parents sit in the same or an earlier page
const R = new Residency(LP, hp.pages);
let topo = true;
for (let g = 0; g < R.groupCount && topo; g++) {
  const { off, list } = R.parents;
  for (let k = off[g]; k < off[g + 1]; k++) if (R.pageOfGroup[list[k]] > R.pageOfGroup[g]) topo = false;
}
check("pages ordered parents first", topo);

// the viewer's cut with residency, on the CPU
function cut(L, res, cam, proj, threshold) {
  const g = new Float32Array(L.groups.buffer);
  const passes = (i) => {
    if (res && !res[i]) return true;
    const e = g[i * 8 + 4];
    if (e > 1e37) return false;
    const d = Math.hypot(g[i * 8] - cam[0], g[i * 8 + 1] - cam[1], g[i * 8 + 2] - cam[2]) - g[i * 8 + 3];
    return e / Math.max(d, 1e-4) * proj * 0.5 <= threshold;
  };
  const out = [];
  for (let i = 0; i < L.clusters.length / 12; i++) {
    const refined = L.clusters[i * 12 + 3] | 0;
    if (!passes(L.clusters[i * 12 + 2]) && (refined < 0 || passes(refined))) out.push(i);
  }
  return out;
}
function openEdges(L, ids) {
  const key = new Map(), edges = new Map();
  const vid = (v) => {
    const k = `${L.vertices[v * 4]},${L.vertices[v * 4 + 1]},${L.vertices[v * 4 + 2]}`;
    let id = key.get(k);
    if (id === undefined) { id = key.size; key.set(k, id); }
    return id;
  };
  let tris = 0;
  for (const c of ids) {
    const vo = L.clusters[c * 12], to = L.clusters[c * 12 + 1], tn = L.clusters[c * 12 + 4] >>> 16;
    for (let t = 0; t < tn; t++) {
      const w = L.triangles[to + t];
      const v = [vid(vo + (w & 255)), vid(vo + ((w >>> 8) & 255)), vid(vo + ((w >>> 16) & 255))];
      for (let j = 0; j < 3; j++) {
        const a = v[j], b = v[(j + 1) % 3], k = a < b ? a * 4294967296 + b : b * 4294967296 + a;
        edges.set(k, (edges.get(k) || 0) + 1);
      }
      tris++;
    }
  }
  let open = 0;
  for (const n of edges.values()) if (n === 1) open++;
  return { open, tris };
}
const lo = hp.aabbMin, hi = hp.aabbMax, size = Math.hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
const views = [[[0, -size * 0.6, 0], 1], [[size * 0.5, 0, size * 0.2], 1], [[0, 0, -size * 2], 2]];
// fully loaded: the same cut as v1
let match = true;
for (const [cam, px] of views) {
  const a = cut(L1, null, cam, 2.414, px / 900), b = cut(LP, R.resident.fill(1), cam, 2.414, px / 900);
  const ta = a.reduce((s, i) => s + (L1.clusters[i * 12 + 4] >>> 16), 0), tb = b.reduce((s, i) => s + (LP.clusters[i * 12 + 4] >>> 16), 0);
  match &&= ta === tb && a.length === b.length;
}
check("fully loaded paged cut equals v1 cut", match);
// pages arriving in random orders (root pages first, as the viewer loads them)
let rng = 12345;
const rand = () => ((rng = (rng * 1103515245 + 12345) >>> 0) / 4294967296);
let allTight = true, allResident = true, steps = 0, partialTris = [];
for (let trial = 0; trial < 3; trial++) {
  const res = new Residency(LP, hp.pages);
  res.markLoaded([...Array(Math.max(1, hp.rootPages)).keys()]);
  const rest = [...Array(hp.pageCount).keys()].slice(Math.max(1, hp.rootPages));
  for (let i = rest.length - 1; i > 0; i--) { const j = Math.floor(rand() * (i + 1)); [rest[i], rest[j]] = [rest[j], rest[i]]; }
  const stride = Math.max(1, Math.floor(rest.length / 6));
  for (let k = 0; k <= rest.length; k += stride) {
    res.markLoaded(rest.slice(Math.max(0, k - stride), k));
    for (const [cam, px] of views) {
      const ids = cut(LP, res.resident, cam, 2.414, px / 900);
      allResident &&= ids.every((i) => res.resident[LP.clusters[i * 12 + 2]] === 1);
      const r = openEdges(LP, ids);
      allTight &&= r.open === 0 && r.tris > 0;
      if (trial === 0 && cam === views[0][0]) partialTris.push(r.tris);
      steps++;
    }
  }
}
check("partial residency: cuts draw only resident clusters", allResident, `${steps} cuts`);
check("partial residency: every cut watertight", allTight, `triangles as pages arrive: ${partialTris.join(" -> ")}`);
console.log(`\n${failed ? "FAILED " + failed : "all passed"}`);
process.exit(failed ? 1 : 0);
