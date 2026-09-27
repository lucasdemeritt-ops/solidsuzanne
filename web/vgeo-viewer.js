// VGEO web viewer: streams a cluster-LOD DAG (.vgeo v2) with WebGPU.
//
// One ES module, no build step, no dependencies:
//   import { createViewer } from "./vgeo-viewer.js";
//   const viewer = await createViewer(canvas, "asset.vgeo", { pixelError: 1 });
//
// Every frame a compute pass decides, per cluster, whether it belongs to the
// cut (the same rule as the Blender add-on: a cluster is drawn when its group
// is too coarse for the view and the group it was simplified from is fine
// enough), culls it against the frustum and appends it to a visible list. One
// indirect instanced draw then renders every visible cluster, pulling vertices
// straight from storage buffers. The CPU never touches the geometry after
// upload, so the cost per frame does not depend on how much the view changed.

const HEADER_BYTES = 200;
const CLUSTER_U32 = 10;   // index_offset, tri_count, group, refined, chunk, depth, center xyz, radius
const GROUP_U32 = 8;      // center xyz, radius, error, depth, reserved x2

export function parseVGEO(buffer) {
  const dv = new DataView(buffer);
  const magic = String.fromCharCode(...new Uint8Array(buffer, 0, 5));
  if (magic !== "VGEO2") throw new Error("not a VGEO v2 file");
  const u32 = (o) => dv.getUint32(o, true);
  const f32 = (o) => dv.getFloat32(o, true);
  const u64 = (o) => Number(dv.getBigUint64(o, true));
  const h = {
    version: u32(8),
    vertexCount: u32(16), indexCount: u32(20), clusterCount: u32(24), groupCount: u32(28),
    chunkCount: u32(32), chunkClusterCount: u32(36), materialCount: u32(40), flags: u32(44),
    lodLevels: u32(48), sourceTriangles: u32(52),
    aabbMin: [f32(56), f32(60), f32(64)], aabbMax: [f32(68), f32(72), f32(76)],
    offPositions: u64(80), offNormals: u64(88), offUVs: u64(96), offVmat: u64(104),
    offIndices: u64(112), offClusters: u64(120), offGroups: u64(128), offChunks: u64(136),
    offChunkClusters: u64(144), offMaterials: u64(152), fileSize: u64(160),
  };
  if (h.version !== 2) throw new Error("unsupported VGEO version " + h.version);
  if (h.fileSize > buffer.byteLength) throw new Error("truncated VGEO file");
  const names = [];
  let p = h.offMaterials;
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
  // widest cluster decides how many vertices each instance of the draw covers
  const clusters = new Uint32Array(buffer, h.offClusters, h.clusterCount * CLUSTER_U32);
  let maxTris = 0;
  for (let i = 0; i < h.clusterCount; i++) maxTris = Math.max(maxTris, clusters[i * CLUSTER_U32 + 1]);
  h.maxClusterTris = maxTris;
  return h;
}

const SELECT_WGSL = /* wgsl */`
struct Cam {
  viewProj : mat4x4f,
  camPos : vec4f,     // xyz, znear
  params : vec4f,     // proj (cot fovy/2), threshold (fraction of height), off-screen scale, cluster count
  planes : array<vec4f, 6>,
};
@group(0) @binding(0) var<uniform> cam : Cam;
@group(0) @binding(1) var<storage, read> clusters : array<u32>;
@group(0) @binding(2) var<storage, read> groups : array<u32>;
@group(0) @binding(3) var<storage, read_write> visible : array<u32>;
@group(0) @binding(4) var<storage, read_write> args : array<atomic<u32>>;

fn inFrustum(c : vec3f, r : f32) -> bool {
  for (var i = 0; i < 6; i++) {
    if (dot(cam.planes[i].xyz, c) + cam.planes[i].w < -r) { return false; }
  }
  return true;
}

fn groupPasses(g : u32) -> bool {
  let b = g * ${GROUP_U32}u;
  let e = bitcast<f32>(groups[b + 4u]);
  if (e > 1e37) { return false; }   // terminal group: never simplified further
  let c = vec3f(bitcast<f32>(groups[b]), bitcast<f32>(groups[b + 1u]), bitcast<f32>(groups[b + 2u]));
  let r = bitcast<f32>(groups[b + 3u]);
  let d = distance(c, cam.camPos.xyz) - r;
  let err = e / max(d, cam.camPos.w) * cam.params.x * 0.5;
  // groups entirely off-screen may be coarser (still a valid cut: their finer groups are off-screen too)
  var t = cam.params.y;
  if (!inFrustum(c, r)) { t = t * cam.params.z; }
  return err <= t;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id : vec3u, @builtin(num_workgroups) nwg : vec3u) {
  let i = id.x + id.y * nwg.x * 64u;
  if (i >= u32(cam.params.w)) { return; }
  let b = i * ${CLUSTER_U32}u;
  let group = clusters[b + 2u];
  let refined = bitcast<i32>(clusters[b + 3u]);
  if (groupPasses(group)) { return; }
  if (refined >= 0 && !groupPasses(u32(refined))) { return; }
  let c = vec3f(bitcast<f32>(clusters[b + 6u]), bitcast<f32>(clusters[b + 7u]), bitcast<f32>(clusters[b + 8u]));
  if (!inFrustum(c, bitcast<f32>(clusters[b + 9u]))) { return; }
  let slot = atomicAdd(&args[1], 1u);
  visible[slot] = i;
  atomicAdd(&args[4], clusters[b + 1u]);   // triangles, for stats
}
`;

const RENDER_WGSL = /* wgsl */`
struct Cam {
  viewProj : mat4x4f,
  camPos : vec4f,
  params : vec4f,
  planes : array<vec4f, 6>,
};
struct Look {
  sunDir : vec4f,     // xyz, exposure
  sunColor : vec4f,   // rgb, mode (0 shaded, 1 lod, 2 clusters, 3 normals)
  skyColor : vec4f,
  groundColor : vec4f,
  fog : vec4f,        // rgb, density
  materials : array<vec4f, 64>,   // base color rgb, roughness
};
@group(0) @binding(0) var<uniform> cam : Cam;
@group(0) @binding(1) var<storage, read> clusters : array<u32>;
@group(0) @binding(2) var<storage, read> indices : array<u32>;
@group(0) @binding(3) var<storage, read> positions : array<f32>;
@group(0) @binding(4) var<storage, read> normals : array<f32>;
@group(0) @binding(5) var<storage, read> vmat : array<u32>;
@group(0) @binding(6) var<storage, read> visible : array<u32>;
@group(0) @binding(7) var<uniform> look : Look;

struct VOut {
  @builtin(position) pos : vec4f,
  @location(0) world : vec3f,
  @location(1) normal : vec3f,
  @location(2) @interpolate(flat) info : vec3u,   // material, lod depth, cluster id
};

@vertex
fn vs(@builtin(vertex_index) vi : u32, @builtin(instance_index) ii : u32) -> VOut {
  var o : VOut;
  let cid = visible[ii];
  let b = cid * ${CLUSTER_U32}u;
  if (vi / 3u >= clusters[b + 1u]) {   // past this cluster's last triangle: degenerate, never rasterized
    o.pos = vec4f(0.0, 0.0, 2.0, 1.0);
    return o;
  }
  let v = indices[clusters[b] + vi];
  let p = vec3f(positions[v * 3u], positions[v * 3u + 1u], positions[v * 3u + 2u]);
  o.world = p;
  o.normal = vec3f(normals[v * 3u], normals[v * 3u + 1u], normals[v * 3u + 2u]);
  o.pos = cam.viewProj * vec4f(p, 1.0);
  let m = (vmat[v >> 1u] >> ((v & 1u) * 16u)) & 0xFFFFu;
  o.info = vec3u(m, clusters[b + 5u], cid);
  return o;
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

@fragment
fn fs(i : VOut, @builtin(front_facing) front : bool) -> @location(0) vec4f {
  var n = normalize(i.normal);
  if (!front) { n = -n; }
  let mode = u32(look.sunColor.w);
  if (mode == 1u) {
    let lod = hash3(i.info.y * 7919u + 13u) * 0.8 + 0.2;
    return vec4f(pow(lod * (0.55 + 0.45 * max(n.z, 0.0)), vec3f(1.0 / 2.2)), 1.0);
  }
  if (mode == 2u) {
    let c = hash3(i.info.z) * 0.8 + 0.2;
    return vec4f(pow(c * (0.55 + 0.45 * max(dot(n, normalize(look.sunDir.xyz)), 0.0)), vec3f(1.0 / 2.2)), 1.0);
  }
  if (mode == 3u) { return vec4f(n * 0.5 + 0.5, 1.0); }
  let mat = look.materials[min(i.info.x, 63u)];
  let albedo = mat.rgb;
  let rough = clamp(mat.a, 0.05, 1.0);
  let l = normalize(look.sunDir.xyz);
  let vdir = normalize(cam.camPos.xyz - i.world);
  let h = normalize(l + vdir);
  let ndl = max(dot(n, l), 0.0);
  let spec = pow(max(dot(n, h), 0.0), mix(256.0, 8.0, rough)) * (1.0 - rough) * 0.35;
  let hemi = mix(look.groundColor.rgb, look.skyColor.rgb, n.z * 0.5 + 0.5);
  var col = albedo * (look.sunColor.rgb * ndl + hemi) + look.sunColor.rgb * spec * ndl;
  // aerial perspective
  let d = distance(cam.camPos.xyz, i.world);
  col = mix(col, look.fog.rgb, 1.0 - exp(-d * look.fog.w));
  col = aces(col * look.sunDir.w);
  return vec4f(pow(col, vec3f(1.0 / 2.2)), 1.0);
}
`;

const DEFAULT_MATERIALS = [
  [0.55, 0.53, 0.5, 0.6], [0.45, 0.55, 0.35, 0.8], [0.55, 0.42, 0.32, 0.7], [0.35, 0.45, 0.6, 0.4],
];

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

function mul4(a, b) {   // column-major a * b
  const o = new Float32Array(16);
  for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
    let s = 0;
    for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
    o[c * 4 + r] = s;
  }
  return o;
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

/**
 * Create a viewer on a canvas.
 * source: URL or ArrayBuffer of a .vgeo file.
 * options: pixelError (1), offscreenScale (8), materials ([{color:[r,g,b], roughness}] or by name),
 *          mode ('shaded' | 'lod' | 'clusters' | 'normals'), onProgress(fraction), onStats(stats),
 *          sun [x,y,z], exposure, fog density, background [r,g,b]
 */
export async function createViewer(canvas, source, options = {}) {
  if (!navigator.gpu) throw new Error("WebGPU is not available in this browser");
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) throw new Error("no WebGPU adapter");
  const buffer = typeof source === "string" ? await fetchWithProgress(source, options.onProgress) : source;
  const h = parseVGEO(buffer);

  const need = Math.max(h.indexCount * 4, h.vertexCount * 12, h.clusterCount * 4) + 256;
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
  const upload = (byteOffset, byteLength, usage = S.STORAGE) => {
    const size = Math.max(16, Math.ceil(byteLength / 4) * 4);
    const b = device.createBuffer({ size, usage: usage | S.COPY_DST, mappedAtCreation: true });
    new Uint8Array(b.getMappedRange()).set(new Uint8Array(buffer, byteOffset, byteLength));
    b.unmap();
    return b;
  };
  const gpu = {
    positions: upload(h.offPositions, h.vertexCount * 12),
    normals: upload(h.offNormals, h.vertexCount * 12),
    vmat: upload(h.offVmat, h.vertexCount * 2),
    indices: upload(h.offIndices, h.indexCount * 4),
    clusters: upload(h.offClusters, h.clusterCount * CLUSTER_U32 * 4),
    groups: upload(h.offGroups, h.groupCount * GROUP_U32 * 4),
  };
  gpu.visible = device.createBuffer({ size: Math.max(16, h.clusterCount * 4), usage: S.STORAGE });
  gpu.args = device.createBuffer({ size: 32, usage: S.STORAGE | S.INDIRECT | S.COPY_DST | S.COPY_SRC });
  gpu.readback = device.createBuffer({ size: 32, usage: S.MAP_READ | S.COPY_DST });
  gpu.cam = device.createBuffer({ size: 256, usage: S.UNIFORM | S.COPY_DST });
  gpu.look = device.createBuffer({ size: 80 + 64 * 16, usage: S.UNIFORM | S.COPY_DST });

  const selectPipeline = device.createComputePipeline({
    layout: "auto",
    compute: { module: device.createShaderModule({ code: SELECT_WGSL }), entryPoint: "main" },
  });
  const renderModule = device.createShaderModule({ code: RENDER_WGSL });
  const renderPipeline = device.createRenderPipeline({
    layout: "auto",
    vertex: { module: renderModule, entryPoint: "vs" },
    fragment: { module: renderModule, entryPoint: "fs", targets: [{ format }] },
    primitive: { topology: "triangle-list", cullMode: "none" },
    depthStencil: { format: "depth32float", depthWriteEnabled: true, depthCompare: "greater" },
  });
  const selectBind = device.createBindGroup({
    layout: selectPipeline.getBindGroupLayout(0),
    entries: [gpu.cam, gpu.clusters, gpu.groups, gpu.visible, gpu.args].map((b, i) => ({ binding: i, resource: { buffer: b } })),
  });
  const renderBind = device.createBindGroup({
    layout: renderPipeline.getBindGroupLayout(0),
    entries: [gpu.cam, gpu.clusters, gpu.indices, gpu.positions, gpu.normals, gpu.vmat, gpu.visible, gpu.look]
      .map((b, i) => ({ binding: i, resource: { buffer: b } })),
  });

  // ---- camera: orbit around the asset, Z up (Blender convention)
  const lo = h.aabbMin, hi = h.aabbMax;
  const center = [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2];
  const radius = Math.hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) / 2 || 1;
  const cam = {
    target: options.target ? [...options.target] : center,
    distance: options.distance ?? radius * 2.2,
    yaw: options.yaw ?? -0.6, pitch: options.pitch ?? 0.35,
    fovy: (options.fov ?? 45) * Math.PI / 180,
  };
  const state = {
    pixelError: options.pixelError ?? 1.0,
    offscreenScale: options.offscreenScale ?? 8.0,
    mode: options.mode ?? "shaded",
    frozen: false, stats: { clusters: 0, triangles: 0, fps: 0, sourceTriangles: h.sourceTriangles },
    running: true, dirty: true,
  };

  const lookData = new Float32Array(20 + 64 * 4);
  const writeLook = () => {
    const sun = options.sun ?? [0.4, -0.6, 0.7];
    const l = Math.hypot(...sun);
    lookData.set([sun[0] / l, sun[1] / l, sun[2] / l, options.exposure ?? 1.0], 0);
    const modes = { shaded: 0, lod: 1, clusters: 2, normals: 3 };
    lookData.set([...(options.sunColor ?? [1.9, 1.8, 1.62]), modes[state.mode] ?? 0], 4);
    lookData.set([...(options.skyColor ?? [0.32, 0.38, 0.48]), 0], 8);
    lookData.set([...(options.groundColor ?? [0.16, 0.14, 0.12]), 0], 12);
    lookData.set([...(options.fogColor ?? [0.62, 0.7, 0.8]), options.fog ?? 0], 16);
    const mats = options.materials || [];
    for (let i = 0; i < 64; i++) {
      let m = Array.isArray(mats) ? mats[i] : mats[h.materialNames[i]];
      if (!m && h.materialParams) m = h.materialParams[i];
      if (!m) m = { color: DEFAULT_MATERIALS[i % DEFAULT_MATERIALS.length].slice(0, 3), roughness: DEFAULT_MATERIALS[i % 4][3] };
      lookData.set([...m.color, m.roughness ?? 0.6], 20 + i * 4);
    }
    device.queue.writeBuffer(gpu.look, 0, lookData);
  };
  writeLook();

  // ---- input
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
    state.dirty = true;
  });
  canvas.addEventListener("pointerup", () => { drag = null; });
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());
  canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    cam.distance *= Math.exp(e.deltaY * 0.001);
    state.dirty = true;
  }, { passive: false });

  let depth = null;
  const camData = new Float32Array(64);
  const args0 = new Uint32Array([h.maxClusterTris * 3, 0, 0, 0, 0, 0, 0, 0]);
  let frame = 0, reading = false, fpsT = performance.now(), fpsN = 0;

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

  function render() {
    if (!state.running) return;
    resize();
    const cp = Math.cos(cam.pitch);
    const eye = [cam.target[0] + cam.distance * cp * Math.cos(cam.yaw + Math.PI / 2),
      cam.target[1] + cam.distance * cp * Math.sin(cam.yaw + Math.PI / 2),
      cam.target[2] + cam.distance * Math.sin(cam.pitch)];
    const near = Math.max(radius * 1e-5, cam.distance * 1e-3);
    const proj = perspectiveReversedZ(cam.fovy, canvas.width / canvas.height, near);
    const view = lookAt(eye, cam.target, [0, 0, 1]);
    const vp = mul4(proj, view);
    camData.set(vp, 0);
    if (!state.frozen) {
      camData.set([...eye, near], 16);
      camData.set([1 / Math.tan(cam.fovy / 2), state.pixelError / canvas.height, state.offscreenScale, h.clusterCount], 20);
      // cull: false keeps every cluster (planes that everything is inside of); used for comparisons
      const planes = options.cull === false ? Array(6).fill([0, 0, 0, 1]) : frustumPlanes(vp);
      for (let i = 0; i < 6; i++) camData.set(planes[i], 24 + i * 4);
      state.view = { eye, near, proj: 1 / Math.tan(cam.fovy / 2), threshold: state.pixelError / canvas.height,
        height: canvas.height };
    }
    device.queue.writeBuffer(gpu.cam, 0, camData);
    device.queue.writeBuffer(gpu.args, 0, args0);

    const enc = device.createCommandEncoder();
    const cpass = enc.beginComputePass();
    cpass.setPipeline(selectPipeline);
    cpass.setBindGroup(0, selectBind);
    const groups = Math.ceil(h.clusterCount / 64);
    const gx = Math.min(groups, 65535);
    cpass.dispatchWorkgroups(gx, Math.ceil(groups / gx));
    cpass.end();
    const bg = options.background ?? [0.62, 0.7, 0.8];
    const rpass = enc.beginRenderPass({
      colorAttachments: [{ view: context.getCurrentTexture().createView(), loadOp: "clear", storeOp: "store",
        clearValue: { r: bg[0], g: bg[1], b: bg[2], a: 1 } }],
      depthStencilAttachment: { view: depth.createView(), depthClearValue: 0, depthLoadOp: "clear", depthStoreOp: "store" },
    });
    rpass.setPipeline(renderPipeline);
    rpass.setBindGroup(0, renderBind);
    rpass.drawIndirect(gpu.args, 0);
    rpass.end();
    const readNow = !reading && frame % 10 === 0;
    if (readNow) enc.copyBufferToBuffer(gpu.args, 0, gpu.readback, 0, 32);
    device.queue.submit([enc.finish()]);
    if (readNow) {
      reading = true;
      gpu.readback.mapAsync(GPUMapMode.READ).then(() => {
        const a = new Uint32Array(gpu.readback.getMappedRange().slice(0));
        gpu.readback.unmap();
        state.stats.clusters = a[1];
        state.stats.triangles = a[4];
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
    header: h,
    device,
    camera: cam,
    get stats() { return state.stats; },
    get view() { return state.view; },
    setPixelError(px) { state.pixelError = px; },
    setMode(m) { state.mode = m; writeLook(); },
    setFrozen(f) { state.frozen = f; },
    setMaterials(m) { options.materials = m; writeLook(); },
    frames() { return frame; },
    destroy() {
      state.running = false;
      Object.values(gpu).forEach((b) => b.destroy?.());
      depth?.destroy();
      device.destroy();
    },
  };
}
