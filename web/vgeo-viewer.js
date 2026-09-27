// VGEO web viewer: streams a cluster-LOD DAG (.vgeow / .vgeo) with WebGPU.
//
// One ES module, no build step:
//   import { createViewer } from "./vgeo-viewer.js";
//   const viewer = await createViewer(canvas, "asset.vgeow", { pixelError: 1 });
// Compressed .vgeow files also need meshopt_decoder.mjs next to this file
// (loaded on demand); .vgeo v2 files need nothing else.
//
// Every frame a compute pass decides, per cluster, whether it belongs to the
// cut (the same rule as the Blender add-on: a cluster is drawn when its group
// is too coarse for the view and the group it was simplified from is fine
// enough), culls it against the frustum and appends it to a visible list. One
// indirect instanced draw then renders every visible cluster, pulling vertices
// straight from storage buffers. The CPU never touches the geometry after
// upload, so the cost per frame does not depend on how much the view changed.
//
// Paged .vgeow files (version 2) stream too: the viewer fetches the head and the
// root pages with HTTP range requests, draws the coarse levels at once, and
// fetches finer pages as the view needs them. A group whose page (or a parent's
// page) is missing counts as fine enough, so its coarser stand-ins are drawn and
// the cut stays crack-free while pages arrive. Servers that ignore range
// requests simply send the whole file.

// GPU layout, shared by both file formats:
//   clusters  12 u32: vertex offset, triangle offset, group, refined, vcount | tcount << 16, depth,
//                     center xyz, radius, 0, 0
//   groups     8 u32: center xyz, radius, error, depth, 0, 0
//   vertices   4 u32: x, y, z on a global 21-bit grid (crack-free), oct normal (snorm16 x 2)
//   triangles  1 u32: i0 | i1 << 8 | i2 << 16 | material << 24 (cluster-local indices)
const CLUSTER_U32 = 12;
const GROUP_U32 = 8;
const PAGE_U32 = 12;
const GRID_MAX = (1 << 21) - 1;

function readMaterials(buffer, dv, p, h) {
  const u32 = (o) => dv.getUint32(o, true);
  const f32 = (o) => dv.getFloat32(o, true);
  const names = [];
  const n = u32(p); p += 4;
  const dec = new TextDecoder();
  for (let i = 0; i < n && p + 4 <= buffer.byteLength; i++) {
    const len = u32(p); p += 4;
    names.push(dec.decode(new Uint8Array(buffer, p, len))); p += len;
  }
  h.materialNames = names;
  // optional "MATP" block: per material base color rgb + roughness
  h.materialParams = null;
  if (p + 4 + n * 16 <= buffer.byteLength && String.fromCharCode(...new Uint8Array(buffer, p, 4)) === "MATP") {
    h.materialParams = [];
    for (let i = 0; i < n; i++) {
      const o = p + 4 + i * 16;
      h.materialParams.push({ color: [f32(o), f32(o + 4), f32(o + 8)], roughness: f32(o + 12) });
    }
  }
}

/** Parse a .vgeo (v2) or .vgeow header. Throws on anything else. */
export function parseVGEO(buffer) {
  const dv = new DataView(buffer);
  const magic = String.fromCharCode(...new Uint8Array(buffer, 0, 5));
  const u32 = (o) => dv.getUint32(o, true);
  const f32 = (o) => dv.getFloat32(o, true);
  const u64 = (o) => Number(dv.getBigUint64(o, true));
  let h;
  if (magic === "VGEO2") {
    h = {
      format: "vgeo", version: u32(8),
      vertexCount: u32(16), indexCount: u32(20), clusterCount: u32(24), groupCount: u32(28),
      materialCount: u32(40), flags: u32(44), lodLevels: u32(48), sourceTriangles: u32(52),
      aabbMin: [f32(56), f32(60), f32(64)], aabbMax: [f32(68), f32(72), f32(76)],
      offPositions: u64(80), offNormals: u64(88), offVmat: u64(104), offIndices: u64(112),
      offClusters: u64(120), offGroups: u64(128), offMaterials: u64(152), fileSize: u64(160),
    };
    if (h.version !== 2) throw new Error("unsupported VGEO version " + h.version);
    const cl = new Uint32Array(buffer, h.offClusters, h.clusterCount * 10);
    let maxTris = 0;
    for (let i = 0; i < h.clusterCount; i++) maxTris = Math.max(maxTris, cl[i * 10 + 1]);
    h.maxClusterTris = maxTris;
  } else if (magic === "VGEOW" && u32(8) === 1) {
    h = {
      format: "vgeow", version: 1,
      clusterCount: u32(16), groupCount: u32(20), vertexCount: u32(24), triangleCount: u32(28),
      materialCount: u32(32), lodLevels: u32(36), sourceTriangles: u32(40), maxClusterTris: u32(44),
      aabbMin: [f32(48), f32(52), f32(56)], aabbMax: [f32(60), f32(64), f32(68)],
      gridOrigin: [f32(72), f32(76), f32(80)], gridStep: f32(84),
      offClusters: u32(88), offGroups: u32(92), offVertices: u32(96), offTriangles: u32(100),
      offMaterials: u32(104), fileSize: u32(108), vertexBytes: u32(112), triangleBytes: u32(116),
      flags: u32(120),
    };
  } else if (magic === "VGEOW" && u32(8) === 2) {
    // paged: the buffer may hold only the head (everything before the first page)
    h = {
      format: "vgeow", version: 2, paged: true,
      clusterCount: u32(16), groupCount: u32(20), vertexCount: u32(24), triangleCount: u32(28),
      materialCount: u32(32), lodLevels: u32(36), sourceTriangles: u32(40), maxClusterTris: u32(44),
      aabbMin: [f32(48), f32(52), f32(56)], aabbMax: [f32(60), f32(64), f32(68)],
      gridOrigin: [f32(72), f32(76), f32(80)], gridStep: f32(84),
      offClusters: u32(88), offGroups: u32(92), offPages: u32(96), pageCount: u32(100),
      offMaterials: u32(104), headBytes: u32(108), fileSize: u64(112), flags: u32(120), rootPages: u32(124),
      clusterBytes: u32(128), groupBytes: u32(132),
    };
    if (buffer.byteLength < h.headBytes) throw new Error("truncated VGEO head");
    const t = new Uint32Array(buffer, h.offPages, h.pageCount * PAGE_U32);
    h.pages = [];
    for (let p = 0; p < h.pageCount; p++) {
      const e = p * PAGE_U32;
      h.pages.push({ g0: t[e], gn: t[e + 1], c0: t[e + 2], cn: t[e + 3], v0: t[e + 4], vn: t[e + 5],
        t0: t[e + 6], tn: t[e + 7], offset: t[e + 8] + t[e + 9] * 2 ** 32, vbytes: t[e + 10], tbytes: t[e + 11] });
    }
    h.partial = buffer.byteLength < h.fileSize;
  } else if (magic === "VGEOW") {
    throw new Error("unsupported VGEOW version " + u32(8));
  } else {
    throw new Error("not a VGEO file");
  }
  if (!h.paged && h.fileSize > buffer.byteLength) throw new Error("truncated VGEO file");
  readMaterials(buffer, dv, h.offMaterials, h);
  return h;
}

// float32 arithmetic and C lround(), so the in-browser conversion of .vgeo files is
// bit-identical to the native exporter's .vgeow
const f = Math.fround;
const lround = (v) => (v < 0 ? -Math.floor(-v + 0.5) : Math.floor(v + 0.5));

function octEncode(x, y, z) {
  const s = f(f(Math.abs(x) + Math.abs(y)) + Math.abs(z));
  if (s <= 0) return 0;
  x = f(x / s); y = f(y / s);
  if (z < 0) {
    const ox = f(f(1 - Math.abs(y)) * (x >= 0 ? 1 : -1));
    const oy = f(f(1 - Math.abs(x)) * (y >= 0 ? 1 : -1));
    x = ox; y = oy;
  }
  const q = (v) => (lround(f(Math.max(-1, Math.min(1, v)) * 32767)) & 0xFFFF);
  return (q(x) | (q(y) << 16)) >>> 0;
}

const DECODER_URL = new URL("./meshopt_decoder.mjs", import.meta.url);

/** Decode one page of a paged .vgeow; `base` is the file offset of buffer[0]. */
export async function decodePage(buffer, page, h, decoderUrl = DECODER_URL, base = 0) {
  const vertices = new Uint32Array(page.vn * 4), triangles = new Uint32Array(page.tn);
  const o = page.offset - base;
  if (h.flags & 1) {
    const { MeshoptDecoder } = await import(decoderUrl.href);
    await MeshoptDecoder.ready;
    if (page.vn) MeshoptDecoder.decodeVertexBuffer(new Uint8Array(vertices.buffer), page.vn, 16, new Uint8Array(buffer, o, page.vbytes));
    if (page.tn) MeshoptDecoder.decodeVertexBuffer(new Uint8Array(triangles.buffer), page.tn, 4, new Uint8Array(buffer, o + page.vbytes, page.tbytes));
  } else {
    vertices.set(new Uint32Array(buffer.slice(o, o + page.vn * 16)));
    triangles.set(new Uint32Array(buffer.slice(o + page.vbytes, o + page.vbytes + page.tn * 4)));
  }
  return { vertices, triangles };
}

/**
 * Which groups of a paged asset the cut may refine into. A group is resident once its page
 * and every parent group (the groups made by simplifying it) are resident: then a missing
 * group always has a drawable coarser stand-in, whatever order pages arrive in.
 */
export class Residency {
  constructor(layout, pages) {
    const n = layout.groups.length / GROUP_U32;
    this.groupCount = n;
    this.pages = pages;
    this.pageOfGroup = new Uint32Array(n);
    pages.forEach((p, i) => this.pageOfGroup.fill(i, p.g0, p.g0 + p.gn));
    // parents[g] = groups of the clusters simplified from g; children = the inverse
    const c = layout.clusters, seen = new Set(), pairs = [];
    for (let i = 0; i < c.length / CLUSTER_U32; i++) {
      const r = c[i * CLUSTER_U32 + 3] | 0, g = c[i * CLUSTER_U32 + 2];
      if (r < 0) continue;
      const k = r * n + g;
      if (!seen.has(k)) { seen.add(k); pairs.push(r, g); }
    }
    const csr = (from, to) => {
      const off = new Uint32Array(n + 1);
      for (let i = 0; i < pairs.length; i += 2) off[pairs[i + from] + 1]++;
      for (let i = 0; i < n; i++) off[i + 1] += off[i];
      const list = new Uint32Array(pairs.length / 2), fill = off.slice(0, n);
      for (let i = 0; i < pairs.length; i += 2) list[fill[pairs[i + from]]++] = pairs[i + to];
      return { off, list };
    };
    this.parents = csr(0, 1);
    this.children = csr(1, 0);
    this.loaded = new Uint8Array(pages.length);
    this.resident = new Uint32Array(n);   // what the GPU cut reads (u32 per group)
    this.residentCount = 0;
  }
  /** Mark pages loaded; returns true if any group became resident. */
  markLoaded(pageIds) {
    const stack = [];
    for (const p of pageIds) {
      this.loaded[p] = 1;
      const pg = this.pages[p];
      for (let g = pg.g0; g < pg.g0 + pg.gn; g++) stack.push(g);
    }
    const before = this.residentCount;
    while (stack.length) {
      const g = stack.pop();
      if (this.resident[g] || !this.loaded[this.pageOfGroup[g]] || !this.parentsResident(g)) continue;
      this.resident[g] = 1;
      this.residentCount++;
      const { off, list } = this.children;
      for (let k = off[g]; k < off[g + 1]; k++) stack.push(list[k]);
    }
    return this.residentCount !== before;
  }
  parentsResident(g) {
    const { off, list } = this.parents;
    for (let k = off[g]; k < off[g + 1]; k++) if (!this.resident[list[k]]) return false;
    return true;
  }
}

/** Decode either format into the shared GPU layout. Paged files that are only partly in
 * `buffer` come back without vertices/triangles (the viewer fills them page by page). */
export async function toGPULayout(buffer, h = parseVGEO(buffer), decoderUrl = DECODER_URL) {
  if (h.paged) {
    let clusters, groups;
    if (h.flags & 2) {
      // encoded head tables (flag bit 1): the same meshopt vertex codec as the pages
      const { MeshoptDecoder } = await import(decoderUrl.href);
      await MeshoptDecoder.ready;
      clusters = new Uint32Array(h.clusterCount * CLUSTER_U32);
      groups = new Uint32Array(h.groupCount * GROUP_U32);
      MeshoptDecoder.decodeVertexBuffer(new Uint8Array(clusters.buffer), h.clusterCount, CLUSTER_U32 * 4,
        new Uint8Array(buffer, h.offClusters, h.clusterBytes));
      MeshoptDecoder.decodeVertexBuffer(new Uint8Array(groups.buffer), h.groupCount, GROUP_U32 * 4,
        new Uint8Array(buffer, h.offGroups, h.groupBytes));
    } else {
      clusters = new Uint32Array(buffer.slice(h.offClusters, h.offClusters + h.clusterCount * CLUSTER_U32 * 4));
      groups = new Uint32Array(buffer.slice(h.offGroups, h.offGroups + h.groupCount * GROUP_U32 * 4));
    }
    const layout = {
      clusters, groups,
      vertices: null, triangles: null, gridOrigin: h.gridOrigin, gridStep: h.gridStep, pages: h.pages,
    };
    if (!h.partial) {
      layout.vertices = new Uint32Array(h.vertexCount * 4);
      layout.triangles = new Uint32Array(h.triangleCount);
      for (const p of h.pages) {
        const d = await decodePage(buffer, p, h, decoderUrl);
        layout.vertices.set(d.vertices, p.v0 * 4);
        layout.triangles.set(d.triangles, p.t0);
      }
    }
    return layout;
  }
  if (h.format === "vgeow") {
    const clusters = new Uint32Array(buffer.slice(h.offClusters, h.offClusters + h.clusterCount * CLUSTER_U32 * 4));
    const groups = new Uint32Array(buffer.slice(h.offGroups, h.offGroups + h.groupCount * GROUP_U32 * 4));
    const vertices = new Uint32Array(h.vertexCount * 4);
    const triangles = new Uint32Array(h.triangleCount);
    if (h.flags & 1) {
      const { MeshoptDecoder } = await import(decoderUrl.href);
      await MeshoptDecoder.ready;
      MeshoptDecoder.decodeVertexBuffer(new Uint8Array(vertices.buffer), h.vertexCount, 16,
        new Uint8Array(buffer, h.offVertices, h.vertexBytes));
      MeshoptDecoder.decodeVertexBuffer(new Uint8Array(triangles.buffer), h.triangleCount, 4,
        new Uint8Array(buffer, h.offTriangles, h.triangleBytes));
    } else {
      vertices.set(new Uint32Array(buffer, h.offVertices, h.vertexCount * 4));
      triangles.set(new Uint32Array(buffer, h.offTriangles, h.triangleCount));
    }
    return { clusters, groups, vertices, triangles, gridOrigin: h.gridOrigin, gridStep: h.gridStep };
  }
  // .vgeo v2: global vertices + global indices -> self-contained clusters on the exporter's grid
  const lo = h.aabbMin, hi = h.aabbMax;
  const extent = Math.max(f(hi[0] - lo[0]), f(hi[1] - lo[1]), f(hi[2] - lo[2]));
  const step = extent > 0 ? f(extent / GRID_MAX) : 1;
  const pos = new Float32Array(buffer, h.offPositions, h.vertexCount * 3);
  const nrm = new Float32Array(buffer, h.offNormals, h.vertexCount * 3);
  const vmat = new Uint16Array(buffer, h.offVmat, h.vertexCount);
  const idx = new Uint32Array(buffer, h.offIndices, h.indexCount);
  const src = new Uint32Array(buffer, h.offClusters, h.clusterCount * 10);
  const clusters = new Uint32Array(h.clusterCount * CLUSTER_U32);
  const triangles = new Uint32Array(h.indexCount / 3);
  let vertices = new Uint32Array(Math.max(16, h.indexCount * 2));
  const local = new Int32Array(h.vertexCount).fill(-1);
  const touched = [];
  let nv = 0, nt = 0;
  for (let c = 0; c < h.clusterCount; c++) {
    const off = src[c * 10], tc = src[c * 10 + 1];
    const vtxOff = nv, triOff = nt;
    touched.length = 0;
    for (let t = 0; t < tc; t++) {
      let word = 0;
      for (let j = 0; j < 3; j++) {
        const v = idx[off + t * 3 + j];
        if (local[v] < 0) {
          local[v] = touched.length;
          touched.push(v);
          if ((nv + 1) * 4 > vertices.length) {
            const grown = new Uint32Array(vertices.length * 2);
            grown.set(vertices);
            vertices = grown;
          }
          for (let k = 0; k < 3; k++) {
            vertices[nv * 4 + k] = lround(Math.max(0, Math.min(GRID_MAX, f(f(pos[v * 3 + k] - lo[k]) / step))));
          }
          vertices[nv * 4 + 3] = octEncode(nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]);
          nv++;
        }
        word |= local[v] << (8 * j);
      }
      triangles[nt++] = (word | (Math.min(vmat[idx[off + t * 3]], 255) << 24)) >>> 0;
    }
    for (const v of touched) local[v] = -1;
    const o = c * CLUSTER_U32;
    clusters[o] = vtxOff;
    clusters[o + 1] = triOff;
    clusters[o + 2] = src[c * 10 + 2];
    clusters[o + 3] = src[c * 10 + 3];
    clusters[o + 4] = (touched.length | (tc << 16)) >>> 0;
    clusters[o + 5] = src[c * 10 + 5];
    for (let k = 0; k < 4; k++) clusters[o + 6 + k] = src[c * 10 + 6 + k];
  }
  const groups = new Uint32Array(buffer.slice(h.offGroups, h.offGroups + h.groupCount * GROUP_U32 * 4));
  return {
    clusters, groups, vertices: vertices.subarray(0, Math.max(4, nv * 4)), triangles: triangles.subarray(0, nt),
    gridOrigin: [...lo], gridStep: step,
  };
}

// ---------------------------------------------------------------- shaders

const ITEM_WGSL = /* wgsl */`
struct Item {
  model : mat4x4f,
  normal : mat4x4f,
  camPos : vec4f,             // camera in asset space, znear (asset units)
  params : vec4f,             // proj (cot fovy/2), threshold (fraction of height), off-screen scale, cluster count
  planes : array<vec4f, 6>,   // frustum in asset space, inward
};
`;

const SELECT_WGSL = ITEM_WGSL + /* wgsl */`
@group(0) @binding(0) var<uniform> item : Item;
@group(0) @binding(1) var<storage, read> clusters : array<u32>;
@group(0) @binding(2) var<storage, read> groups : array<u32>;
@group(0) @binding(3) var<storage, read_write> visible : array<u32>;
@group(0) @binding(4) var<storage, read_write> args : array<atomic<u32>>;
@group(0) @binding(5) var<storage, read> resident : array<u32>;   // per group (paged files)

fn inFrustum(c : vec3f, r : f32) -> bool {
  for (var i = 0; i < 6; i++) {
    if (dot(item.planes[i].xyz, c) + item.planes[i].w < -r) { return false; }
  }
  return true;
}

fn groupPasses(g : u32) -> bool {
  // not loaded yet: stop refining here, the coarser clusters made from it stand in
  if (resident[g] == 0u) { return true; }
  let b = g * ${GROUP_U32}u;
  let e = bitcast<f32>(groups[b + 4u]);
  if (e > 1e37) { return false; }   // terminal group: never simplified further
  let c = vec3f(bitcast<f32>(groups[b]), bitcast<f32>(groups[b + 1u]), bitcast<f32>(groups[b + 2u]));
  let r = bitcast<f32>(groups[b + 3u]);
  let d = distance(c, item.camPos.xyz) - r;
  let err = e / max(d, item.camPos.w) * item.params.x * 0.5;
  // groups entirely off-screen may be coarser (still a valid cut: their finer groups are off-screen too)
  var t = item.params.y;
  if (!inFrustum(c, r)) { t = t * item.params.z; }
  return err <= t;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id : vec3u, @builtin(num_workgroups) nwg : vec3u) {
  let i = id.x + id.y * nwg.x * 64u;
  if (i >= u32(item.params.w)) { return; }
  let b = i * ${CLUSTER_U32}u;
  let group = clusters[b + 2u];
  let refined = bitcast<i32>(clusters[b + 3u]);
  if (groupPasses(group)) { return; }
  if (refined >= 0 && !groupPasses(u32(refined))) { return; }
  let c = vec3f(bitcast<f32>(clusters[b + 6u]), bitcast<f32>(clusters[b + 7u]), bitcast<f32>(clusters[b + 8u]));
  if (!inFrustum(c, bitcast<f32>(clusters[b + 9u]))) { return; }
  let slot = atomicAdd(&args[1], 1u);
  visible[slot] = i;
  atomicAdd(&args[4], clusters[b + 4u] >> 16u);   // triangles, for stats
}
`;

const COMMON_WGSL = /* wgsl */`
struct Global {
  viewProj : mat4x4f,
  eye : vec4f,          // camera xyz, exposure
  sunDir : vec4f,       // xyz, mode (0 shaded, 1 lod, 2 clusters, 3 normals)
  sunColor : vec4f,
  skyColor : vec4f,
  groundColor : vec4f,
  fog : vec4f,          // rgb, density
};
struct AssetLook {
  grid : vec4f,                   // quantization grid origin xyz, step
  materials : array<vec4f, 64>,   // base color rgb, roughness
};
struct VOut {
  @builtin(position) pos : vec4f,
  @location(0) world : vec3f,
  @location(1) normal : vec3f,
  @location(2) @interpolate(flat) info : vec3u,   // material, lod depth, cluster id
};

fn octDecode(w : u32) -> vec3f {
  let x = f32(bitcast<i32>(w << 16u) >> 16u) / 32767.0;
  let y = f32(bitcast<i32>(w) >> 16u) / 32767.0;
  var n = vec3f(x, y, 1.0 - abs(x) - abs(y));
  if (n.z < 0.0) {
    n = vec3f((1.0 - abs(n.y)) * select(-1.0, 1.0, n.x >= 0.0), (1.0 - abs(n.x)) * select(-1.0, 1.0, n.y >= 0.0), n.z);
  }
  return normalize(n);
}

fn hash3(x : u32) -> vec3f {
  var h = x * 747796405u + 2891336453u;
  h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
  h = (h >> 22u) ^ h;
  return vec3f(f32(h & 255u), f32((h >> 8u) & 255u), f32((h >> 16u) & 255u)) / 255.0;
}

fn aces(x : vec3f) -> vec3f {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3f(0.0), vec3f(1.0));
}

// fetch one vertex of a cluster: local position; triangle word and packed normal via pointers
fn fetchVertex(b : u32, vi : u32, out_tri : ptr<function, u32>, out_n : ptr<function, u32>) -> vec3f {
  let t = triangles[clusters[b + 1u] + vi / 3u];
  *out_tri = t;
  let v = (clusters[b] + ((t >> (8u * (vi % 3u))) & 255u)) * 4u;
  *out_n = vertices[v + 3u];
  return asset.grid.xyz + vec3f(f32(vertices[v]), f32(vertices[v + 1u]), f32(vertices[v + 2u])) * asset.grid.w;
}

fn shade(i : VOut, front : bool) -> vec4f {
  var n = normalize(i.normal);
  if (!front) { n = -n; }
  let mode = u32(g.sunDir.w);
  if (mode == 1u) {
    let lod = hash3(i.info.y * 7919u + 13u) * 0.8 + 0.2;
    return vec4f(pow(lod * (0.55 + 0.45 * max(n.z, 0.0)), vec3f(1.0 / 2.2)), 1.0);
  }
  if (mode == 2u) {
    let c = hash3(i.info.z) * 0.8 + 0.2;
    return vec4f(pow(c * (0.55 + 0.45 * max(dot(n, normalize(g.sunDir.xyz)), 0.0)), vec3f(1.0 / 2.2)), 1.0);
  }
  if (mode == 3u) { return vec4f(n * 0.5 + 0.5, 1.0); }
  let mat = asset.materials[min(i.info.x, 63u)];
  let rough = clamp(mat.a, 0.05, 1.0);
  let l = normalize(g.sunDir.xyz);
  let vdir = normalize(g.eye.xyz - i.world);
  let h = normalize(l + vdir);
  let ndl = max(dot(n, l), 0.0);
  let spec = pow(max(dot(n, h), 0.0), mix(256.0, 8.0, rough)) * (1.0 - rough) * 0.35;
  let hemi = mix(g.groundColor.rgb, g.skyColor.rgb, n.z * 0.5 + 0.5);
  var col = mat.rgb * (g.sunColor.rgb * ndl + hemi) + g.sunColor.rgb * spec * ndl;
  let d = distance(g.eye.xyz, i.world);   // aerial perspective
  col = mix(col, g.fog.rgb, 1.0 - exp(-d * g.fog.w));
  col = aces(col * g.eye.w);
  return vec4f(pow(col, vec3f(1.0 / 2.2)), 1.0);
}
`;

// streamed placement: clusters come from the compute pass, one model matrix
const STREAMED_WGSL = ITEM_WGSL + /* wgsl */`
@group(0) @binding(0) var<uniform> g : Global;
@group(0) @binding(1) var<uniform> item : Item;
@group(0) @binding(2) var<storage, read> clusters : array<u32>;
@group(0) @binding(3) var<storage, read> triangles : array<u32>;
@group(0) @binding(4) var<storage, read> vertices : array<u32>;
@group(0) @binding(5) var<storage, read> visible : array<u32>;
@group(0) @binding(6) var<uniform> asset : AssetLook;
` + COMMON_WGSL + /* wgsl */`
@vertex
fn vs(@builtin(vertex_index) vi : u32, @builtin(instance_index) ii : u32) -> VOut {
  var o : VOut;
  let cid = visible[ii];
  let b = cid * ${CLUSTER_U32}u;
  if (vi / 3u >= (clusters[b + 4u] >> 16u)) {   // past this cluster's last triangle: degenerate
    o.pos = vec4f(0.0, 0.0, 2.0, 1.0);
    return o;
  }
  var t : u32;
  var nw : u32;
  let pv = fetchVertex(b, vi, &t, &nw);
  let world = item.model * vec4f(pv, 1.0);
  o.world = world.xyz;
  o.normal = (item.normal * vec4f(octDecode(nw), 0.0)).xyz;
  o.pos = g.viewProj * world;
  o.info = vec3u(t >> 24u, clusters[b + 5u], cid);
  return o;
}
@fragment
fn fs(i : VOut, @builtin(front_facing) front : bool) -> @location(0) vec4f { return shade(i, front); }
`;

// instanced placements: each copy shows one whole-asset level; one draw per level
const INSTANCED_WGSL = /* wgsl */`
struct Draw { levelOffset : u32, clusterCount : u32, instOffset : u32, pad : u32 };
@group(0) @binding(0) var<uniform> g : Global;
@group(0) @binding(1) var<uniform> draw : Draw;
@group(0) @binding(2) var<storage, read> clusters : array<u32>;
@group(0) @binding(3) var<storage, read> triangles : array<u32>;
@group(0) @binding(4) var<storage, read> vertices : array<u32>;
@group(0) @binding(5) var<storage, read> levelClusters : array<u32>;
@group(0) @binding(6) var<uniform> asset : AssetLook;
@group(0) @binding(7) var<storage, read> matrices : array<mat4x4f>;   // model, normal per placement
@group(0) @binding(8) var<storage, read> instList : array<u32>;
` + COMMON_WGSL + /* wgsl */`
@vertex
fn vs(@builtin(vertex_index) vi : u32, @builtin(instance_index) ii : u32) -> VOut {
  var o : VOut;
  let inst = instList[draw.instOffset + ii / draw.clusterCount];
  let cid = levelClusters[draw.levelOffset + ii % draw.clusterCount];
  let b = cid * ${CLUSTER_U32}u;
  if (vi / 3u >= (clusters[b + 4u] >> 16u)) {
    o.pos = vec4f(0.0, 0.0, 2.0, 1.0);
    return o;
  }
  var t : u32;
  var nw : u32;
  let pv = fetchVertex(b, vi, &t, &nw);
  let world = matrices[inst * 2u] * vec4f(pv, 1.0);
  o.world = world.xyz;
  o.normal = (matrices[inst * 2u + 1u] * vec4f(octDecode(nw), 0.0)).xyz;
  o.pos = g.viewProj * world;
  o.info = vec3u(t >> 24u, clusters[b + 5u], cid ^ (inst * 2654435761u));
  return o;
}
@fragment
fn fs(i : VOut, @builtin(front_facing) front : bool) -> @location(0) vec4f { return shade(i, front); }
`;

const DEFAULT_MATERIALS = [
  [0.55, 0.53, 0.5, 0.6], [0.45, 0.55, 0.35, 0.8], [0.55, 0.42, 0.32, 0.7], [0.35, 0.45, 0.6, 0.4],
];

// ---------------------------------------------------------------- math (column-major 4x4)

const IDENTITY = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

function perspectiveReversedZ(fovy, aspect, near) {
  // infinite far plane, depth 1 at near and 0 at infinity (precision stays even for huge scenes)
  const f = 1 / Math.tan(fovy / 2);
  return new Float32Array([f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, 0, -1, 0, 0, near, 0]);
}

function lookAt(eye, target, up) {
  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const norm = (a) => { const l = Math.hypot(...a) || 1; return a.map((x) => x / l); };
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const z = norm(sub(eye, target));
  const x = norm(cross(up, z));
  const y = cross(z, x);
  return new Float32Array([x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
    -dot(x, eye), -dot(y, eye), -dot(z, eye), 1]);
}

function mul4(a, b) {
  const o = new Float32Array(16);
  for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
    let s = 0;
    for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
    o[c * 4 + r] = s;
  }
  return o;
}

function transformPoint(m, p) {
  return [m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
    m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
    m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]];
}

function maxScale(m) {
  return Math.max(Math.hypot(m[0], m[1], m[2]), Math.hypot(m[4], m[5], m[6]), Math.hypot(m[8], m[9], m[10])) || 1;
}

function invert4(m) {
  const inv = new Float64Array(16);
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  let det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (!det) return new Float32Array(IDENTITY);
  det = 1 / det;
  return Float32Array.from(inv, (v) => v * det);
}

function normalMatrix(m) {   // inverse transpose of the upper 3x3, as a mat4
  const inv = invert4(m);
  const n = new Float32Array(16);
  for (let c = 0; c < 3; c++) for (let r = 0; r < 3; r++) n[c * 4 + r] = inv[r * 4 + c];
  n[15] = 1;
  return n;
}

function frustumPlanes(m) {
  // Gribb-Hartmann for a column-major matrix and WebGPU clip space (0 <= z <= w)
  const row = (r) => [m[r], m[4 + r], m[8 + r], m[12 + r]];
  const r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
  const add = (a, b) => a.map((x, i) => x + b[i]);
  const sub = (a, b) => a.map((x, i) => x - b[i]);
  const planes = [add(r3, r0), sub(r3, r0), add(r3, r1), sub(r3, r1), r2, sub(r3, r2)];
  return planes.map((p) => { const l = Math.hypot(p[0], p[1], p[2]) || 1; return p.map((x) => x / l); });
}

function planesToLocal(planes, model) {
  // plane p (world) in asset space: p_local[c] = sum_r p[r] * M[r][c]
  return planes.map((p) => {
    const q = [0, 0, 0, 0];
    for (let c = 0; c < 4; c++) q[c] = p[0] * model[c * 4] + p[1] * model[c * 4 + 1] + p[2] * model[c * 4 + 2] + p[3] * model[c * 4 + 3];
    const l = Math.hypot(q[0], q[1], q[2]) || 1;
    return q.map((x) => x / l);
  });
}

async function fetchWithProgress(url, onProgress) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const total = Number(res.headers.get("content-length")) || 0;
  if (!res.body || !total) return res.arrayBuffer();
  const out = new Uint8Array(total);
  const reader = res.body.getReader();
  let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    out.set(value, got);
    got += value.length;
    onProgress?.(got / total);
  }
  return out.buffer;
}

/** bytes [start, end) of a URL. partial: false when the server ignored the range (whole file). */
async function fetchRange(url, start, end) {
  const res = await fetch(url, { headers: { Range: `bytes=${start}-${end - 1}` } });
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  return { buffer: await res.arrayBuffer(), partial: res.status === 206 };
}

function concat(a, b) {
  const out = new Uint8Array(a.byteLength + b.byteLength);
  out.set(new Uint8Array(a), 0);
  out.set(new Uint8Array(b), a.byteLength);
  return out.buffer;
}

/** Whole-asset levels (what one instanced copy can show): error, clusters and triangles per level. */
export function levelTables(layout) {
  const c = layout.clusters, g = new Float32Array(layout.groups.buffer, layout.groups.byteOffset, layout.groups.length);
  const n = c.length / CLUSTER_U32;
  let depthMax = 0;
  for (let i = 0; i < n; i++) depthMax = Math.max(depthMax, c[i * CLUSTER_U32 + 5]);
  const levels = depthMax + 1;
  const errors = new Float64Array(levels);
  for (let i = 0; i < n; i++) {
    const refined = c[i * CLUSTER_U32 + 3] | 0;
    if (refined < 0) continue;
    const e = g[refined * GROUP_U32 + 4];
    const d = c[i * CLUSTER_U32 + 5];
    if (e < 1e37 && e > errors[d]) errors[d] = e;
  }
  for (let L = 1; L < levels; L++) errors[L] = Math.max(errors[L], errors[L - 1]);
  const lists = [], tris = new Float64Array(levels);
  for (let L = 0; L < levels; L++) {
    const ids = [];
    for (let i = 0; i < n; i++) {
      const d = c[i * CLUSTER_U32 + 5];
      const terminal = g[c[i * CLUSTER_U32 + 2] * GROUP_U32 + 4] > 1e37;
      if (d === L || (d < L && terminal)) { ids.push(i); tris[L] += c[i * CLUSTER_U32 + 4] >>> 16; }
    }
    lists.push(ids);
  }
  const offsets = new Uint32Array(levels), counts = new Uint32Array(levels);
  let total = 0;
  lists.forEach((ids, L) => { offsets[L] = total; counts[L] = ids.length; total += ids.length; });
  const flat = new Uint32Array(Math.max(1, total));
  lists.forEach((ids, L) => flat.set(ids, offsets[L]));
  return { errors, offsets, counts, clusters: flat, tris };
}

// ---------------------------------------------------------------- viewer

// a copy seen up close wants level 0 (the whole asset at full detail); from this size on it gets
// a streamed cut of its own instead (fine where you look, coarse elsewhere)
const STREAM_MIN_TRIS = 20000;

/**
 * Create a viewer on a canvas.
 * source: URL or ArrayBuffer of a .vgeow/.vgeo asset, or a scene: URL of a .json file or an object
 *   { objects: [{ src, matrix?: [16 column-major], instances?: url-of-float32-bin | [[16], ...] }] }
 *   (one placement streams its own cut; several placements pick a whole-asset level each, and the
 *   nearest copies that want full detail get a streamed cut of their own).
 * options: pixelError (1), offscreenScale (8), materials ([{color:[r,g,b], roughness}] or by name),
 *          mode ('shaded' | 'lod' | 'clusters' | 'normals'), onProgress(fraction), onStats(stats),
 *          sun [x,y,z], exposure, fog density, background [r,g,b], interactive (true),
 *          camera start: target [x,y,z], distance, yaw, pitch, fov (vertical degrees) or fovX
 *          (horizontal degrees, vertical follows the canvas aspect), decoderUrl,
 *          streamedCopies (4: instanced copies that may stream their own cut),
 *          streaming (true: fetch paged .vgeow files by range as the view needs them),
 *          maxRequests (4: page requests in flight)
 */
export async function createViewer(canvas, source, options = {}) {
  if (!navigator.gpu) throw new Error("WebGPU is not available in this browser");
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) throw new Error("no WebGPU adapter");

  // ---- scene description
  let scene, base = location.href;
  if (typeof source === "string" && /\.json(\?|#|$)/i.test(source)) {
    base = new URL(source, location.href).href;
    const res = await fetch(source);
    if (!res.ok) throw new Error(`${source}: HTTP ${res.status}`);
    scene = await res.json();
  } else if (source && !(source instanceof ArrayBuffer) && typeof source === "object" && source.objects) {
    scene = source;
  } else {
    scene = { objects: [{ src: source }] };
  }
  if (!scene.objects?.length) throw new Error("scene has no objects");
  const decoderUrl = options.decoderUrl ? new URL(options.decoderUrl, location.href) : DECODER_URL;
  const resolve = (u) => (typeof u === "string" ? new URL(u, base).href : u);

  // ---- load assets (each once) and placements
  const loaded = new Map();
  const nObj = scene.objects.length;
  let doneObj = 0;
  const netStats = { bytes: 0 };
  // Paged .vgeow over HTTP: the head, then the root pages (every group nothing coarser can stand
  // in for); finer pages come later. Anything else (or a server without ranges): the whole file.
  const whole = async (buffer) => {
    const h = parseVGEO(buffer);
    return { h, layout: await toGPULayout(buffer, h, decoderUrl) };
  };
  const loadPaged = async (url) => {
    const first = await fetchRange(url, 0, 65536);
    netStats.bytes += first.buffer.byteLength;
    if (!first.partial) return whole(first.buffer);   // no range support: that was the whole file
    const dv = new DataView(first.buffer);
    const magic = String.fromCharCode(...new Uint8Array(first.buffer, 0, 5));
    if (first.buffer.byteLength < 128 || magic !== "VGEOW" || dv.getUint32(8, true) !== 2) return null;
    if (first.buffer.byteLength >= Number(dv.getBigUint64(112, true))) return whole(first.buffer);   // small file
    let head = first.buffer;
    const headBytes = dv.getUint32(108, true);
    if (head.byteLength < headBytes) {
      const rest = await fetchRange(url, head.byteLength, headBytes);
      netStats.bytes += rest.buffer.byteLength;
      head = concat(head, rest.buffer);
    }
    const h = parseVGEO(head.byteLength > headBytes ? head.slice(0, headBytes) : head);
    const layout = await toGPULayout(head, h, decoderUrl);
    const root = Math.max(1, h.rootPages);
    const pg = h.pages.slice(0, root);
    const end = pg.length ? pg[pg.length - 1].offset + pg[pg.length - 1].vbytes + pg[pg.length - 1].tbytes : headBytes;
    let rootBuf = head.byteLength > headBytes ? head.slice(headBytes) : new ArrayBuffer(0);
    const have = headBytes + rootBuf.byteLength;
    if (end > have) {
      const r = await fetchRange(url, have, end);
      netStats.bytes += r.buffer.byteLength;
      rootBuf = concat(rootBuf, r.buffer);
    }
    return { h, layout, paged: { url, rootPages: pg.map((_, i) => i), rootBuf, rootBase: headBytes } };
  };
  const loadAsset = async (src) => {
    const key = typeof src === "string" ? resolve(src) : src;
    if (!loaded.has(key)) {
      loaded.set(key, (async () => {
        if (typeof key === "string" && options.streaming !== false) {
          try {
            const p = await loadPaged(key);
            if (p) return p;
          } catch (e) {
            if (!/HTTP/.test(e.message)) throw e;   // e.g. 416 on a tiny file: fall back to a plain fetch
          }
        }
        const buffer = typeof key === "string"
          ? await fetchWithProgress(key, (f) => options.onProgress?.((doneObj + f) / nObj)) : key;
        if (typeof key === "string") netStats.bytes += buffer.byteLength;
        const h = parseVGEO(buffer);
        const layout = await toGPULayout(buffer, h, decoderUrl);
        return { h, layout };
      })());
    }
    return loaded.get(key);
  };
  const specs = [];
  for (const o of scene.objects) {
    const a = await loadAsset(o.src);
    let mats = [];
    if (typeof o.instances === "string") {
      const res = await fetch(resolve(o.instances));
      if (!res.ok) throw new Error(`${o.instances}: HTTP ${res.status}`);
      const f = new Float32Array(await res.arrayBuffer());
      for (let i = 0; i + 16 <= f.length; i += 16) mats.push(f.slice(i, i + 16));
    } else if (Array.isArray(o.instances)) {
      mats = o.instances.map((m) => new Float32Array(m));
    } else {
      mats = [o.matrix ? new Float32Array(o.matrix) : new Float32Array(IDENTITY)];
    }
    specs.push({ asset: a, matrices: mats });
    doneObj++;
    options.onProgress?.(doneObj / nObj);
  }

  let need = 256;
  for (const { h, layout } of await Promise.all(loaded.values())) {
    const vb = layout.vertices ? layout.vertices.byteLength : h.vertexCount * 16;
    const tb = layout.triangles ? layout.triangles.byteLength : h.triangleCount * 4;
    need = Math.max(need, vb, tb, layout.clusters.byteLength);
  }
  if (need > adapter.limits.maxStorageBufferBindingSize) {
    throw new Error(`asset needs ${(need / 2 ** 20).toFixed(0)} MB buffers; this GPU allows ` +
      `${(adapter.limits.maxStorageBufferBindingSize / 2 ** 20).toFixed(0)} MB`);
  }
  const device = await adapter.requestDevice({
    requiredLimits: {
      maxStorageBufferBindingSize: adapter.limits.maxStorageBufferBindingSize,
      maxBufferSize: adapter.limits.maxBufferSize,
      maxStorageBuffersPerShaderStage: Math.min(8, adapter.limits.maxStorageBuffersPerShaderStage),
    },
  });
  const context = canvas.getContext("webgpu");
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: "opaque" });

  const S = GPUBufferUsage;
  const owned = [];
  const upload = (array, usage = S.STORAGE) => {
    const size = Math.max(16, Math.ceil(array.byteLength / 4) * 4);
    const b = device.createBuffer({ size, usage: usage | S.COPY_DST, mappedAtCreation: true });
    new Uint8Array(b.getMappedRange()).set(new Uint8Array(array.buffer, array.byteOffset, array.byteLength));
    b.unmap();
    owned.push(b);
    return b;
  };
  const buffer = (size, usage) => {
    const b = device.createBuffer({ size: Math.max(16, Math.ceil(size / 4) * 4), usage });
    owned.push(b);
    return b;
  };

  const selectPipeline = device.createComputePipeline({
    layout: "auto", compute: { module: device.createShaderModule({ code: SELECT_WGSL }), entryPoint: "main" },
  });
  const renderPipeline = (code) => {
    const module = device.createShaderModule({ code });
    return device.createRenderPipeline({
      layout: "auto",
      vertex: { module, entryPoint: "vs" },
      fragment: { module, entryPoint: "fs", targets: [{ format }] },
      primitive: { topology: "triangle-list", cullMode: "none" },
      depthStencil: { format: "depth32float", depthWriteEnabled: true, depthCompare: "greater" },
    });
  };
  const streamedPipeline = renderPipeline(STREAMED_WGSL);
  const instancedPipeline = renderPipeline(INSTANCED_WGSL);
  const globalBuf = buffer(192, S.UNIFORM | S.COPY_DST);
  const bind = (pipeline, list) => device.createBindGroup({
    layout: pipeline.getBindGroupLayout(0),
    entries: list.map((b, i) => ({ binding: i, resource: { buffer: b } })),
  });

  // ---- per asset GPU data
  const assets = [];
  const gpuAssets = new Map();
  const assetGPU = (a) => {
    if (gpuAssets.has(a)) return gpuAssets.get(a);
    const { h, layout } = a;
    const groupCount = layout.groups.length / GROUP_U32;
    const g = {
      h, layout,
      clusters: upload(layout.clusters), groups: upload(layout.groups),
      vertices: layout.vertices ? upload(layout.vertices) : buffer(h.vertexCount * 16, S.STORAGE | S.COPY_DST),
      triangles: layout.triangles ? upload(layout.triangles) : buffer(h.triangleCount * 4, S.STORAGE | S.COPY_DST),
      look: buffer(16 + 64 * 16, S.UNIFORM | S.COPY_DST),
      lo: h.aabbMin, hi: h.aabbMax,
      groupData: new Float32Array(layout.groups.buffer, layout.groups.byteOffset, layout.groups.length),
    };
    if (a.paged) {
      const res = new Residency(layout, h.pages);
      g.paged = { url: a.paged.url, res, pages: h.pages, requested: new Uint8Array(h.pages.length),
        inflight: 0, dirty: false, levelsDirty: true, finest: 0 };
      g.resident = buffer(groupCount * 4, S.STORAGE | S.COPY_DST);
      g.pendingPages = { buf: a.paged.rootBuf, base: a.paged.rootBase, ids: a.paged.rootPages };
    } else {
      g.resident = upload(new Uint32Array(groupCount).fill(1));
    }
    g.center = [0, 1, 2].map((k) => (g.lo[k] + g.hi[k]) / 2);
    g.radius = Math.hypot(g.hi[0] - g.lo[0], g.hi[1] - g.lo[1], g.hi[2] - g.lo[2]) / 2 || 1;
    gpuAssets.set(a, g);
    assets.push(g);
    return g;
  };
  const writeAssetLook = (g) => {
    const d = new Float32Array(4 + 64 * 4);
    d.set([...g.layout.gridOrigin, g.layout.gridStep], 0);
    const mats = options.materials || [];
    for (let i = 0; i < 64; i++) {
      let m = Array.isArray(mats) ? mats[i] : mats[g.h.materialNames[i]];
      if (!m && g.h.materialParams) m = g.h.materialParams[i];
      if (!m) m = { color: DEFAULT_MATERIALS[i % 4].slice(0, 3), roughness: DEFAULT_MATERIALS[i % 4][3] };
      d.set([...m.color, m.roughness ?? 0.6], 4 + i * 4);
    }
    device.queue.writeBuffer(g.look, 0, d);
  };
  // pages that arrived: decode, write into the full-size streams, then publish residency
  const installPages = async (g, buf, base, ids) => {
    for (const id of ids) {
      const p = g.paged.pages[id];
      const d = await decodePage(buf, p, g.h, decoderUrl, base);
      if (p.vn) device.queue.writeBuffer(g.vertices, p.v0 * 16, d.vertices);
      if (p.tn) device.queue.writeBuffer(g.triangles, p.t0 * 4, d.triangles);
    }
    if (g.paged.res.markLoaded(ids)) {
      device.queue.writeBuffer(g.resident, 0, g.paged.res.resident);
      g.paged.levelsDirty = true;
    }
  };

  // a streamed placement: its own cut, selected on the GPU every frame
  const makeStreamer = (g) => {
    const st = { kind: "streamed", g };
    st.uniform = buffer(256, S.UNIFORM | S.COPY_DST);
    st.visible = buffer(g.h.clusterCount * 4, S.STORAGE);
    st.args = buffer(32, S.STORAGE | S.INDIRECT | S.COPY_DST | S.COPY_SRC);
    st.data = new Float32Array(64);
    st.selectBind = bind(selectPipeline, [st.uniform, g.clusters, g.groups, st.visible, st.args, g.resident]);
    st.renderBind = bind(streamedPipeline, [globalBuf, st.uniform, g.clusters, g.triangles, g.vertices, st.visible, g.look]);
    st.setModel = (m) => {
      st.model = m;
      st.normal = normalMatrix(m);
      st.scale = maxScale(m);
      st.inv = invert4(m);
      st.data.set(m, 0);
      st.data.set(st.normal, 16);
    };
    return st;
  };

  // ---- draw items
  const items = [];
  let sourceTriangles = 0;
  let maxTris = 0;
  const wlo = [Infinity, Infinity, Infinity], whi = [-Infinity, -Infinity, -Infinity];
  for (const spec of specs) {
    const g = assetGPU(spec.asset);
    maxTris = Math.max(maxTris, g.h.maxClusterTris);
    sourceTriangles += g.h.sourceTriangles * spec.matrices.length;
    for (const m of spec.matrices) {
      for (let k = 0; k < 8; k++) {
        const p = transformPoint(m, [k & 1 ? g.hi[0] : g.lo[0], k & 2 ? g.hi[1] : g.lo[1], k & 4 ? g.hi[2] : g.lo[2]]);
        for (let a = 0; a < 3; a++) { wlo[a] = Math.min(wlo[a], p[a]); whi[a] = Math.max(whi[a], p[a]); }
      }
    }
    if (spec.matrices.length === 1) {
      const it = makeStreamer(g);
      it.setModel(spec.matrices[0]);
      it.active = true;
      items.push(it);
    } else {
      if (!g.levels) {
        g.levels = levelTables(g.layout);
        g.levelClusters = upload(g.levels.clusters);
      }
      const n = spec.matrices.length;
      const it = { kind: "instanced", g, n, models: spec.matrices };
      const mats = new Float32Array(n * 32);
      it.center = new Float32Array(n * 3);
      it.scale = new Float32Array(n);
      spec.matrices.forEach((m, i) => {
        mats.set(m, i * 32);
        mats.set(normalMatrix(m), i * 32 + 16);
        it.center.set(transformPoint(m, g.center), i * 3);
        it.scale[i] = maxScale(m);
      });
      it.matrices = upload(mats);
      it.instList = buffer(n * 4, S.STORAGE | S.COPY_DST);
      it.list = new Uint32Array(n);
      it.level = new Int32Array(n);
      it.budget = new Float64Array(n);
      it.draws = [];
      const L = g.levels.errors.length;
      for (let l = 0; l < L; l++) {
        const u = buffer(16, S.UNIFORM | S.COPY_DST);
        it.draws.push({ u, bind: bind(instancedPipeline,
          [globalBuf, u, g.clusters, g.triangles, g.vertices, g.levelClusters, g.look, it.matrices, it.instList]) });
      }
      const k = g.levels.tris[0] >= STREAM_MIN_TRIS ? Math.max(0, options.streamedCopies ?? 4) : 0;
      it.slots = Array.from({ length: Math.min(k, n) }, () => makeStreamer(g));
      items.push(it);
    }
  }
  assets.forEach(writeAssetLook);
  for (const g of assets) {
    if (g.pendingPages) {
      await installPages(g, g.pendingPages.buf, g.pendingPages.base, g.pendingPages.ids);
      for (const id of g.pendingPages.ids) g.paged.requested[id] = 1;
      delete g.pendingPages;
    }
  }
  const streamerCount = items.reduce((n, it) => n + (it.kind === "streamed" ? 1 : it.slots.length), 0);
  const readback = buffer(32 * Math.max(1, streamerCount), S.MAP_READ | S.COPY_DST);

  // ---- camera: orbit, Z up (Blender convention)
  const center = [0, 1, 2].map((k) => (wlo[k] + whi[k]) / 2);
  const radius = Math.hypot(whi[0] - wlo[0], whi[1] - wlo[1], whi[2] - wlo[2]) / 2 || 1;
  const cam = {
    target: options.target ? [...options.target] : center,
    distance: options.distance ?? radius * 2.2,
    yaw: options.yaw ?? -0.6, pitch: options.pitch ?? 0.35,
    fovy: (options.fov ?? 45) * Math.PI / 180,
  };
  let fovX = options.fovX || 0;
  const pageTotal = assets.reduce((n, g) => n + (g.paged ? g.paged.pages.length : 0), 0);
  const state = {
    pixelError: options.pixelError ?? 1.0,
    offscreenScale: options.offscreenScale ?? 8.0,
    mode: options.mode ?? "shaded",
    frozen: false, running: true,
    stats: { clusters: 0, triangles: 0, fps: 0, sourceTriangles, instances: 0, streamedCopies: 0,
      bytesLoaded: 0, pagesLoaded: 0, pagesTotal: pageTotal },
  };
  const modes = { shaded: 0, lod: 1, clusters: 2, normals: 3 };
  const globalData = new Float32Array(48);

  // ---- input (options.interactive === false: a fixed view, e.g. a hero image on a page)
  if (options.interactive !== false) {
    let drag = null;
    canvas.style.touchAction = "none";
    canvas.addEventListener("pointerdown", (e) => {
      drag = { x: e.clientX, y: e.clientY, pan: e.button === 2 || e.shiftKey };
      canvas.setPointerCapture(e.pointerId);
    });
    canvas.addEventListener("pointermove", (e) => {
      if (!drag) return;
      const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
      drag.x = e.clientX; drag.y = e.clientY;
      if (drag.pan) {
        const s = cam.distance * 0.0015;
        const cy = Math.cos(cam.yaw), sy = Math.sin(cam.yaw);
        cam.target[0] -= (cy * dx) * s; cam.target[1] -= (sy * dx) * s;
        cam.target[2] += dy * s;
      } else {
        cam.yaw -= dx * 0.005;
        cam.pitch = Math.max(-1.5, Math.min(1.5, cam.pitch + dy * 0.005));
      }
    });
    canvas.addEventListener("pointerup", () => { drag = null; });
    canvas.addEventListener("contextmenu", (e) => e.preventDefault());
    canvas.addEventListener("wheel", (e) => {
      e.preventDefault();
      cam.distance *= Math.exp(e.deltaY * 0.001);
    }, { passive: false });
  }

  let depth = null;
  let frame = 0, reading = false, fpsT = performance.now(), fpsN = 0;
  let instTris = 0, instCount = 0;

  function resize() {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.round(canvas.clientWidth * dpr));
    const hgt = Math.max(1, Math.round(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== hgt || !depth) {
      canvas.width = w; canvas.height = hgt;
      depth?.destroy();
      depth = device.createTexture({ size: [w, hgt], format: "depth32float", usage: GPUTextureUsage.RENDER_ATTACHMENT });
    }
  }

  // the view as a streamed placement sees it (asset space), for the GPU cut and page requests
  function updateStreamer(st, eye, worldPlanes, near, proj, threshold) {
    const local = transformPoint(st.inv, eye);
    st.data.set([...local, near / st.scale], 32);
    st.data.set([proj, threshold, state.offscreenScale, st.g.h.clusterCount], 36);
    const pl = options.cull === false ? worldPlanes : planesToLocal(worldPlanes, st.model);
    for (let i = 0; i < 6; i++) st.data.set(pl[i], 40 + i * 4);
    device.queue.writeBuffer(st.uniform, 0, st.data);
  }

  // finest whole-asset level whose clusters are all resident (paged assets)
  function finestResident(g) {
    const pg = g.paged;
    if (!pg.levelsDirty) return pg.finest;
    pg.levelsDirty = false;
    const { offsets, counts, clusters } = g.levels, res = pg.res.resident, c = g.layout.clusters;
    let finest = counts.length - 1;
    for (let L = counts.length - 1; L >= 0; L--) {
      let ok = true;
      for (let k = offsets[L]; k < offsets[L] + counts[L] && ok; k++) ok = res[c[clusters[k] * CLUSTER_U32 + 2]] === 1;
      if (!ok) break;
      finest = L;
    }
    pg.finest = finest;
    return finest;
  }

  function chooseLevels(it, eye, planes, near, proj, threshold) {
    const g = it.g, errors = g.levels.errors, L = errors.length;
    const floor = g.paged ? finestResident(g) : 0;
    const counts = new Uint32Array(L);
    const want0 = [];
    let visibleN = 0;
    for (let i = 0; i < it.n; i++) {
      const cx = it.center[i * 3], cy = it.center[i * 3 + 1], cz = it.center[i * 3 + 2];
      const r = g.radius * it.scale[i];
      let inside = true;
      for (let p = 0; p < 6 && inside; p++) inside = planes[p][0] * cx + planes[p][1] * cy + planes[p][2] * cz + planes[p][3] >= -r;
      if (!inside) { it.level[i] = -1; continue; }
      const d = Math.max(Math.hypot(cx - eye[0], cy - eye[1], cz - eye[2]) - r, near);
      const budget = threshold * d / (proj * 0.5) / it.scale[i];
      let l = 0;
      while (l + 1 < L && errors[l + 1] <= budget) l++;
      if (l < floor) g.paged.wantFiner = true;
      if (l === 0 && it.slots.length) want0.push(i);
      it.budget[i] = budget;
      it.level[i] = Math.max(l, floor);
    }
    // the nearest copies that want full detail stream a cut of their own
    want0.sort((a, b) => it.budget[a] - it.budget[b]);
    it.slots.forEach((st, k) => {
      const i = want0[k];
      st.active = i !== undefined;
      if (!st.active) return;
      if (st.placement !== i) { st.placement = i; st.setModel(it.models[i]); }
      it.level[i] = -1;
      updateStreamer(st, eye, planes, near, proj, threshold);
    });
    for (let i = 0; i < it.n; i++) if (it.level[i] >= 0) { counts[it.level[i]]++; visibleN++; }
    const offs = new Uint32Array(L);
    for (let l = 1; l < L; l++) offs[l] = offs[l - 1] + counts[l - 1];
    const fill = offs.slice();
    for (let i = 0; i < it.n; i++) if (it.level[i] >= 0) it.list[fill[it.level[i]]++] = i;
    device.queue.writeBuffer(it.instList, 0, it.list, 0, Math.max(1, visibleN));
    it.active = [];
    for (let l = 0; l < L; l++) {
      if (!counts[l]) continue;
      device.queue.writeBuffer(it.draws[l].u, 0, new Uint32Array([g.levels.offsets[l], g.levels.counts[l], offs[l], 0]));
      it.active.push({ l, count: counts[l] });
      instTris += g.levels.tris[l] * counts[l];
    }
    instCount += visibleN + it.slots.filter((s) => s.active).length;
  }

  // ---- page streaming: which missing groups the current views refine into, most urgent first
  function wantedPages(g, streamers) {
    const pg = g.paged, res = pg.res, gd = g.groupData, want = new Map();
    if (res.residentCount === res.groupCount) return [];
    for (const st of streamers) {
      const d = st.data, ex = d[32], ey = d[33], ez = d[34], zn = d[35], proj = d[36], t0 = d[37], off = d[38];
      for (let k = 0; k < res.groupCount; k++) {
        if (res.resident[k] || pg.requested[res.pageOfGroup[k]] || !res.parentsResident(k)) continue;
        const b = k * GROUP_U32, e = gd[b + 4];
        if (e > 1e37) continue;
        const cx = gd[b], cy = gd[b + 1], cz = gd[b + 2], r = gd[b + 3];
        let t = t0;
        for (let p = 0; p < 6; p++) {
          if (d[40 + p * 4] * cx + d[41 + p * 4] * cy + d[42 + p * 4] * cz + d[43 + p * 4] < -r) { t *= off; break; }
        }
        const err = e / Math.max(Math.hypot(cx - ex, cy - ey, cz - ez) - r, zn) * proj * 0.5;
        if (err <= t) continue;   // fine enough: its members are not needed
        const p = res.pageOfGroup[k], u = err / t;
        if ((want.get(p) ?? 0) < u) want.set(p, u);
      }
    }
    if (pg.wantFiner) {   // instanced copies: next pages in coarse-to-fine order
      let n = 0;
      for (let p = 0; p < pg.pages.length && n < 8; p++) if (!pg.requested[p]) { if (!want.has(p)) want.set(p, 0.5); n++; }
    }
    return [...want.entries()].sort((a, b) => b[1] - a[1]).map(([p]) => p);
  }

  function schedulePages(streamersByAsset) {
    const maxReq = Math.max(1, options.maxRequests ?? 4);
    for (const g of assets) {
      const pg = g.paged;
      if (!pg || pg.inflight >= maxReq) continue;
      const order = wantedPages(g, streamersByAsset.get(g) || []);
      pg.wantFiner = false;
      const taken = new Set();
      for (const p0 of order) {
        if (pg.inflight >= maxReq) break;
        if (taken.has(p0) || pg.requested[p0]) continue;
        // extend to neighbouring wanted pages: one range, up to 512 KB
        const wantSet = new Set(order);
        let a = p0, b = p0;
        const size = (x, y) => pg.pages[y].offset + pg.pages[y].vbytes + pg.pages[y].tbytes - pg.pages[x].offset;
        while (b + 1 < pg.pages.length && wantSet.has(b + 1) && !pg.requested[b + 1] && size(a, b + 1) < 524288) b++;
        while (a > 0 && wantSet.has(a - 1) && !pg.requested[a - 1] && size(a - 1, b) < 524288) a--;
        const ids = [];
        for (let p = a; p <= b; p++) { ids.push(p); pg.requested[p] = 1; taken.add(p); }
        pg.inflight++;
        const start = pg.pages[a].offset, end = start + size(a, b);
        fetchRange(pg.url, start, end).then(async ({ buffer: buf, partial }) => {
          netStats.bytes += buf.byteLength;
          if (!partial) {   // the server sent the whole file: take every page from it
            const all = pg.pages.map((_, i) => i).filter((i) => !pg.res.loaded[i]);
            all.forEach((i) => { pg.requested[i] = 1; });
            await installPages(g, buf, 0, all);
          } else {
            await installPages(g, buf, start, ids);
          }
        }).catch((e) => {
          ids.forEach((p) => { pg.requested[p] = 0; });   // retry later
          console.warn("VGEO page fetch failed:", e.message || e);
        }).finally(() => { pg.inflight--; });
      }
    }
  }

  function render() {
    if (!state.running) return;
    resize();
    const aspect = canvas.width / canvas.height;
    if (fovX) cam.fovy = 2 * Math.atan(Math.tan(fovX * Math.PI / 360) / aspect);
    const cp = Math.cos(cam.pitch);
    const eye = [cam.target[0] + cam.distance * cp * Math.cos(cam.yaw + Math.PI / 2),
      cam.target[1] + cam.distance * cp * Math.sin(cam.yaw + Math.PI / 2),
      cam.target[2] + cam.distance * Math.sin(cam.pitch)];
    const near = Math.max(radius * 1e-5, cam.distance * 1e-3);
    const vp = mul4(perspectiveReversedZ(cam.fovy, aspect, near), lookAt(eye, cam.target, [0, 0, 1]));
    const proj = 1 / Math.tan(cam.fovy / 2);
    const threshold = state.pixelError / canvas.height;
    const sun = options.sun ?? [0.4, -0.6, 0.7];
    const sl = Math.hypot(...sun);
    globalData.set(vp, 0);
    globalData.set([...eye, options.exposure ?? 1.0], 16);
    globalData.set([sun[0] / sl, sun[1] / sl, sun[2] / sl, modes[state.mode] ?? 0], 20);
    globalData.set([...(options.sunColor ?? [1.9, 1.8, 1.62]), 0], 24);
    globalData.set([...(options.skyColor ?? [0.32, 0.38, 0.48]), 0], 28);
    globalData.set([...(options.groundColor ?? [0.16, 0.14, 0.12]), 0], 32);
    globalData.set([...(options.fogColor ?? [0.62, 0.7, 0.8]), options.fog ?? 0], 36);
    device.queue.writeBuffer(globalBuf, 0, globalData);

    const worldPlanes = options.cull === false ? Array(6).fill([0, 0, 0, 1]) : frustumPlanes(vp);
    if (!state.frozen) {
      state.view = { eye, near, proj, threshold, height: canvas.height };
      instTris = 0; instCount = 0;
      for (const it of items) {
        if (it.kind === "streamed") updateStreamer(it, eye, worldPlanes, near, proj, threshold);
        else chooseLevels(it, eye, worldPlanes, near, proj, threshold);
      }
    }
    // everything that runs the GPU cut this frame: streamed items and active streamed copies
    const streamers = [];
    for (const it of items) {
      if (it.kind === "streamed") streamers.push(it);
      else for (const st of it.slots) if (st.active) streamers.push(st);
    }
    if (!state.frozen && pageTotal && frame % 3 === 0) {
      const byAsset = new Map();
      for (const st of streamers) {
        if (!byAsset.has(st.g)) byAsset.set(st.g, []);
        byAsset.get(st.g).push(st);
      }
      schedulePages(byAsset);
    }
    const args0 = new Uint32Array([maxTris * 3, 0, 0, 0, 0, 0, 0, 0]);
    if (!state.frozen) for (const st of streamers) device.queue.writeBuffer(st.args, 0, args0);

    const enc = device.createCommandEncoder();
    if (!state.frozen && streamers.length) {
      const cpass = enc.beginComputePass();
      cpass.setPipeline(selectPipeline);
      for (const st of streamers) {
        cpass.setBindGroup(0, st.selectBind);
        const groups = Math.ceil(st.g.h.clusterCount / 64);
        const gx = Math.min(groups, 65535);
        cpass.dispatchWorkgroups(gx, Math.ceil(groups / gx));
      }
      cpass.end();
    }
    const bg = options.background ?? [0.62, 0.7, 0.8];
    const rpass = enc.beginRenderPass({
      colorAttachments: [{ view: context.getCurrentTexture().createView(), loadOp: "clear", storeOp: "store",
        clearValue: { r: bg[0], g: bg[1], b: bg[2], a: 1 } }],
      depthStencilAttachment: { view: depth.createView(), depthClearValue: 0, depthLoadOp: "clear", depthStoreOp: "store" },
    });
    for (const st of streamers) {
      rpass.setPipeline(streamedPipeline);
      rpass.setBindGroup(0, st.renderBind);
      rpass.drawIndirect(st.args, 0);
    }
    for (const it of items) {
      if (it.kind !== "instanced" || !it.active) continue;
      rpass.setPipeline(instancedPipeline);
      for (const { l, count } of it.active) {
        rpass.setBindGroup(0, it.draws[l].bind);
        rpass.draw(maxTris * 3, it.g.levels.counts[l] * count);
      }
    }
    rpass.end();
    const readNow = !reading && frame % 10 === 0;
    if (readNow) streamers.forEach((st, k) => enc.copyBufferToBuffer(st.args, 0, readback, k * 32, 32));
    device.queue.submit([enc.finish()]);
    if (readNow) {
      reading = true;
      const it2 = instTris, ic2 = instCount, n = streamers.length;
      const copies = streamers.filter((st) => items.indexOf(st) < 0).length;
      readback.mapAsync(GPUMapMode.READ).then(() => {
        const a = new Uint32Array(readback.getMappedRange().slice(0));
        readback.unmap();
        let cl = 0, tr = 0;
        for (let k = 0; k < n; k++) { cl += a[k * 8 + 1]; tr += a[k * 8 + 4]; }
        state.stats.clusters = cl;
        state.stats.triangles = tr + it2;
        state.stats.instances = ic2;
        state.stats.streamedCopies = copies;
        state.stats.bytesLoaded = netStats.bytes;
        state.stats.pagesLoaded = assets.reduce((s, g) => s + (g.paged ? g.paged.res.loaded.reduce((x, y) => x + y, 0) : 0), 0);
        reading = false;
        options.onStats?.(state.stats);
      }).catch(() => { reading = false; });
    }
    frame++;
    fpsN++;
    const now = performance.now();
    if (now - fpsT > 500) { state.stats.fps = fpsN * 1000 / (now - fpsT); fpsT = now; fpsN = 0; }
    requestAnimationFrame(render);
  }
  requestAnimationFrame(render);

  return {
    header: assets[0].h,
    assets: assets.map((g) => g.h),
    device,
    camera: cam,
    get stats() {
      state.stats.bytesLoaded = netStats.bytes;
      state.stats.pagesLoaded = assets.reduce((s, g) => s + (g.paged ? g.paged.res.loaded.reduce((x, y) => x + y, 0) : 0), 0);
      return state.stats;
    },
    get view() { return state.view; },
    setPixelError(px) { state.pixelError = px; },
    setMode(m) { state.mode = m; },
    setFrozen(f) { state.frozen = f; },
    setMaterials(m) { options.materials = m; assets.forEach(writeAssetLook); },
    /** Jump to a pose: { target, distance, yaw, pitch, fov | fovX } (any subset). */
    setView(pose) {
      if (pose.target) cam.target = [...pose.target];
      if (pose.distance !== undefined) cam.distance = pose.distance;
      if (pose.yaw !== undefined) cam.yaw = pose.yaw;
      if (pose.pitch !== undefined) cam.pitch = pose.pitch;
      if (pose.fovX) fovX = pose.fovX;
      else if (pose.fov) { fovX = 0; cam.fovy = pose.fov * Math.PI / 180; }
    },
    frames() { return frame; },
    destroy() {
      state.running = false;
      owned.forEach((b) => b.destroy());
      depth?.destroy();
      device.destroy();
    },
  };
}
