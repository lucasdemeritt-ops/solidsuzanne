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

// GPU layout, shared by both file formats:
//   clusters  12 u32: vertex offset, triangle offset, group, refined, vcount | tcount << 16, depth,
//                     center xyz, radius, 0, 0
//   groups     8 u32: center xyz, radius, error, depth, 0, 0
//   vertices   4 u32: x, y, z on a global 21-bit grid (crack-free), oct normal (snorm16 x 2)
//   triangles  1 u32: i0 | i1 << 8 | i2 << 16 | material << 24 (cluster-local indices)
const CLUSTER_U32 = 12;
const GROUP_U32 = 8;
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
  } else if (magic === "VGEOW") {
    h = {
      format: "vgeow", version: u32(8),
      clusterCount: u32(16), groupCount: u32(20), vertexCount: u32(24), triangleCount: u32(28),
      materialCount: u32(32), lodLevels: u32(36), sourceTriangles: u32(40), maxClusterTris: u32(44),
      aabbMin: [f32(48), f32(52), f32(56)], aabbMax: [f32(60), f32(64), f32(68)],
      gridOrigin: [f32(72), f32(76), f32(80)], gridStep: f32(84),
      offClusters: u32(88), offGroups: u32(92), offVertices: u32(96), offTriangles: u32(100),
      offMaterials: u32(104), fileSize: u32(108), vertexBytes: u32(112), triangleBytes: u32(116),
      flags: u32(120),
    };
    if (h.version !== 1) throw new Error("unsupported VGEOW version " + h.version);
  } else {
    throw new Error("not a VGEO file");
  }
  if (h.fileSize > buffer.byteLength) throw new Error("truncated VGEO file");
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

/** Decode either format into the shared GPU layout. */
export async function toGPULayout(buffer, h = parseVGEO(buffer),
                                  decoderUrl = new URL("./meshopt_decoder.mjs", import.meta.url)) {
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

fn inFrustum(c : vec3f, r : f32) -> bool {
  for (var i = 0; i < 6; i++) {
    if (dot(item.planes[i].xyz, c) + item.planes[i].w < -r) { return false; }
  }
  return true;
}

fn groupPasses(g : u32) -> bool {
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

/**
 * Create a viewer on a canvas.
 * source: URL or ArrayBuffer of a .vgeow/.vgeo asset, or a scene: URL of a .json file or an object
 *   { objects: [{ src, matrix?: [16 column-major], instances?: url-of-float32-bin | [[16], ...] }] }
 *   (one placement streams its own cut; several placements pick a whole-asset level each).
 * options: pixelError (1), offscreenScale (8), materials ([{color:[r,g,b], roughness}] or by name),
 *          mode ('shaded' | 'lod' | 'clusters' | 'normals'), onProgress(fraction), onStats(stats),
 *          sun [x,y,z], exposure, fog density, background [r,g,b], interactive (true),
 *          camera start: target [x,y,z], distance, yaw, pitch, fov (vertical degrees) or fovX
 *          (horizontal degrees, vertical follows the canvas aspect), decoderUrl
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
  const decoderUrl = options.decoderUrl ? new URL(options.decoderUrl, location.href) : undefined;
  const resolve = (u) => (typeof u === "string" ? new URL(u, base).href : u);

  // ---- load assets (each once) and placements
  const loaded = new Map();
  const nObj = scene.objects.length;
  let doneObj = 0;
  const loadAsset = async (src) => {
    const key = typeof src === "string" ? resolve(src) : src;
    if (!loaded.has(key)) {
      loaded.set(key, (async () => {
        const buffer = typeof key === "string"
          ? await fetchWithProgress(key, (f) => options.onProgress?.((doneObj + f) / nObj)) : key;
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
  for (const { layout } of await Promise.all(loaded.values())) {
    need = Math.max(need, layout.vertices.byteLength, layout.triangles.byteLength, layout.clusters.byteLength);
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
  const buffer = (size, usage) => { const b = device.createBuffer({ size: Math.max(16, size), usage }); owned.push(b); return b; };

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
    const g = {
      h, layout,
      clusters: upload(layout.clusters), groups: upload(layout.groups),
      vertices: upload(layout.vertices), triangles: upload(layout.triangles),
      look: buffer(16 + 64 * 16, S.UNIFORM | S.COPY_DST),
      lo: h.aabbMin, hi: h.aabbMax,
    };
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
      const it = { kind: "streamed", g, model: spec.matrices[0] };
      it.normal = normalMatrix(it.model);
      it.scale = maxScale(it.model);
      it.inv = invert4(it.model);
      it.uniform = buffer(256, S.UNIFORM | S.COPY_DST);
      it.visible = buffer(g.h.clusterCount * 4, S.STORAGE);
      it.args = buffer(32, S.STORAGE | S.INDIRECT | S.COPY_DST | S.COPY_SRC);
      it.data = new Float32Array(64);
      it.data.set(it.model, 0);
      it.data.set(it.normal, 16);
      it.selectBind = bind(selectPipeline, [it.uniform, g.clusters, g.groups, it.visible, it.args]);
      it.renderBind = bind(streamedPipeline, [globalBuf, it.uniform, g.clusters, g.triangles, g.vertices, it.visible, g.look]);
      items.push(it);
    } else {
      if (!g.levels) {
        g.levels = levelTables(g.layout);
        g.levelClusters = upload(g.levels.clusters);
      }
      const n = spec.matrices.length;
      const it = { kind: "instanced", g, n };
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
      it.draws = [];
      const L = g.levels.errors.length;
      for (let l = 0; l < L; l++) {
        const u = buffer(16, S.UNIFORM | S.COPY_DST);
        it.draws.push({ u, bind: bind(instancedPipeline,
          [globalBuf, u, g.clusters, g.triangles, g.vertices, g.levelClusters, g.look, it.matrices, it.instList]) });
      }
      items.push(it);
    }
  }
  assets.forEach(writeAssetLook);
  const readback = buffer(32 * Math.max(1, items.length), S.MAP_READ | S.COPY_DST);

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
  const state = {
    pixelError: options.pixelError ?? 1.0,
    offscreenScale: options.offscreenScale ?? 8.0,
    mode: options.mode ?? "shaded",
    frozen: false, running: true,
    stats: { clusters: 0, triangles: 0, fps: 0, sourceTriangles, instances: 0 },
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

  function chooseLevels(it, eye, planes, near, proj, threshold) {
    const g = it.g, errors = g.levels.errors, L = errors.length;
    const counts = new Uint32Array(L);
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
      it.level[i] = l;
      counts[l]++;
      visibleN++;
    }
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
    instCount += visibleN;
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
        if (it.kind === "streamed") {
          const local = transformPoint(it.inv, eye);
          it.data.set([...local, near / it.scale], 32);
          it.data.set([proj, threshold, state.offscreenScale, it.g.h.clusterCount], 36);
          const pl = options.cull === false ? worldPlanes : planesToLocal(worldPlanes, it.model);
          for (let i = 0; i < 6; i++) it.data.set(pl[i], 40 + i * 4);
          device.queue.writeBuffer(it.uniform, 0, it.data);
        } else {
          chooseLevels(it, eye, worldPlanes, near, proj, threshold);
        }
      }
    }
    const args0 = new Uint32Array([maxTris * 3, 0, 0, 0, 0, 0, 0, 0]);
    for (const it of items) if (it.kind === "streamed" && !state.frozen) device.queue.writeBuffer(it.args, 0, args0);

    const enc = device.createCommandEncoder();
    if (!state.frozen) {
      const cpass = enc.beginComputePass();
      cpass.setPipeline(selectPipeline);
      for (const it of items) {
        if (it.kind !== "streamed") continue;
        cpass.setBindGroup(0, it.selectBind);
        const groups = Math.ceil(it.g.h.clusterCount / 64);
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
    for (const it of items) {
      if (it.kind === "streamed") {
        rpass.setPipeline(streamedPipeline);
        rpass.setBindGroup(0, it.renderBind);
        rpass.drawIndirect(it.args, 0);
      } else if (it.active) {
        rpass.setPipeline(instancedPipeline);
        for (const { l, count } of it.active) {
          rpass.setBindGroup(0, it.draws[l].bind);
          rpass.draw(maxTris * 3, it.g.levels.counts[l] * count);
        }
      }
    }
    rpass.end();
    const streamedItems = items.filter((it) => it.kind === "streamed");
    const readNow = !reading && frame % 10 === 0;
    if (readNow) streamedItems.forEach((it, k) => enc.copyBufferToBuffer(it.args, 0, readback, k * 32, 32));
    device.queue.submit([enc.finish()]);
    if (readNow) {
      reading = true;
      const it2 = instTris, ic2 = instCount;
      readback.mapAsync(GPUMapMode.READ).then(() => {
        const a = new Uint32Array(readback.getMappedRange().slice(0));
        readback.unmap();
        let cl = 0, tr = 0;
        streamedItems.forEach((_, k) => { cl += a[k * 8 + 1]; tr += a[k * 8 + 4]; });
        state.stats.clusters = cl;
        state.stats.triangles = tr + it2;
        state.stats.instances = ic2;
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
    get stats() { return state.stats; },
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
