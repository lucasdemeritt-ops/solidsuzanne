// Browser checks for paged streaming and streamed copies (headless Edge, real GPU):
//   python tests/web/make_paged_assets.py
//   node tests/web/paged_check.mjs --puppeteer <dir with node_modules/puppeteer-core> [--out DIR]
// Serves the repo with range requests (and once without), opens tests/web/paged.html and checks:
// the paged terrain shows its coarse levels after a small download and fetches only what a
// close-up needs; settled cuts equal the v1 file's; a server without ranges still works; the
// copy nearest the camera in an instanced scene streams its own, lighter cut.
import { createRequire } from "node:module";
import { mkdirSync, readFileSync, statSync } from "node:fs";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { startServer } from "./range_server.mjs";

const argv = process.argv.slice(2);
const arg = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const REPO = resolve(fileURLToPath(new URL("../..", import.meta.url)));
const OUT = resolve(arg("--out", join(REPO, "build-demo", "paged_check")));
mkdirSync(OUT, { recursive: true });
const require = createRequire(join(resolve(arg("--puppeteer", ".")), "index.js"));
const puppeteer = require("puppeteer-core");
const EDGE = arg("--browser", "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe");

let failed = 0;
const check = (name, ok, detail = "") => { console.log((ok ? "PASS " : "FAIL ") + name + (detail ? `  [${detail}]` : "")); if (!ok) failed++; };
const MB = (b) => (b / 2 ** 20).toFixed(2) + " MB";

const ranged = await startServer({ port: 8781, root: REPO });
const plain = await startServer({ port: 8782, root: REPO, ranges: false });
const browser = await puppeteer.launch({
  executablePath: EDGE, headless: "new",
  args: ["--enable-unsafe-webgpu", "--use-angle=d3d11", "--ignore-gpu-blocklist", "--window-size=1000,700"],
  defaultViewport: { width: 1000, height: 700 },
});

async function open(port, src, opts = {}) {
  const page = await browser.newPage();
  page.on("pageerror", (e) => console.log("pageerror:", e.message));
  page.on("console", (m) => { if (m.type() === "error" || m.type() === "warning") console.log("console:", m.text()); });
  const url = `http://127.0.0.1:${port}/tests/web/paged.html?src=${encodeURIComponent(src)}&opts=${encodeURIComponent(JSON.stringify(opts))}`;
  await page.goto(url, { waitUntil: "load" });
  await page.waitForFunction("window.viewer || window.viewerError", { timeout: 120000 });
  const err = await page.evaluate("window.viewerError");
  if (err) throw new Error(err);
  return page;
}
const stats = (page) => page.evaluate(() => ({ ...window.viewer.stats, loadMs: window.loadMs, frames: window.viewer.frames() }));
// wait until triangles and loaded pages stop changing
async function settle(page, quietMs = 1500, maxMs = 60000) {
  const t0 = Date.now();
  let last = "", since = Date.now(), s;
  for (;;) {
    await new Promise((r) => setTimeout(r, 200));
    s = await stats(page);
    const key = `${s.triangles}|${s.pagesLoaded}|${s.streamedCopies}`;
    if (key !== last) { last = key; since = Date.now(); }
    if (Date.now() - since > quietMs && s.frames > 30) return s;
    if (Date.now() - t0 > maxMs) return s;
  }
}
const setView = (page, v) => page.evaluate((v) => window.viewer.setView(v), v);

// ---- terrain: paged vs v1, wide and close
const terrainBytes = statSync(join(REPO, "web/assets/terrain2m_paged.vgeow")).size;
const hdr = await (async () => {
  const b = readFileSync(join(REPO, "web/assets/terrain2m_paged.vgeow"));
  return { headBytes: b.readUInt32LE(108), lo: [0, 4, 8].map((o) => b.readFloatLE(48 + o)), hi: [0, 4, 8].map((o) => b.readFloatLE(60 + o)) };
})();
const size = Math.hypot(...hdr.hi.map((v, k) => v - hdr.lo[k]));
const wide = { yaw: -0.6, pitch: 0.35, distance: size * 1.1 };
const center = hdr.lo.map((v, k) => (v + hdr.hi[k]) / 2);
const close = { target: [center[0] + size * 0.08, center[1] - size * 0.05, hdr.lo[2] + (hdr.hi[2] - hdr.lo[2]) * 0.55],
  yaw: 0.4, pitch: 0.25, distance: size * 0.07 };

await fetch("http://127.0.0.1:8781/__reset");
const tp = await open(8781, "../../web/assets/terrain2m_paged.vgeow", wide);
const first = await stats(tp);
check("paged terrain opens after head + root pages", first.bytesLoaded < terrainBytes * 0.2 && first.pagesLoaded >= 1,
  `${MB(first.bytesLoaded)} of ${MB(terrainBytes)} before the first frame, ${first.loadMs.toFixed(0)} ms, head ${MB(hdr.headBytes)}`);
const pw = await settle(tp);
await tp.screenshot({ path: join(OUT, "terrain_paged_wide.png") });
await setView(tp, close);
const pc = await settle(tp);
await tp.screenshot({ path: join(OUT, "terrain_paged_close.png") });
const served = await (await fetch("http://127.0.0.1:8781/__stats")).json();
check("close-up fetched finer pages on demand", pc.pagesLoaded > pw.pagesLoaded && pc.triangles > 0,
  `pages ${pw.pagesLoaded} -> ${pc.pagesLoaded} of ${pc.pagesTotal}; triangles ${pw.triangles.toLocaleString()} -> ${pc.triangles.toLocaleString()}`);
check("only part of the file was downloaded", pc.bytesLoaded < terrainBytes * 0.8,
  `${MB(pc.bytesLoaded)} of ${MB(terrainBytes)} (${served.ranges} range requests)`);
await tp.close();

const t1 = await open(8781, "../../web/assets/terrain2m.vgeow", wide);
const vw = await settle(t1);
await setView(t1, close);
const vc = await settle(t1);
await t1.screenshot({ path: join(OUT, "terrain_v1_close.png") });
await t1.close();
check("settled paged cut equals the v1 cut (wide)", pw.triangles === vw.triangles && pw.clusters === vw.clusters,
  `${pw.triangles.toLocaleString()} vs ${vw.triangles.toLocaleString()}`);
check("settled paged cut equals the v1 cut (close)", pc.triangles === vc.triangles && pc.clusters === vc.clusters,
  `${pc.triangles.toLocaleString()} vs ${vc.triangles.toLocaleString()}`);

// ---- a server without range requests: the whole file, same picture
const np = await open(8782, "../../web/assets/terrain2m_paged.vgeow", wide);
const nw = await settle(np);
await np.close();
check("server without ranges: whole file, same cut", nw.bytesLoaded >= terrainBytes && nw.triangles === vw.triangles,
  `${MB(nw.bytesLoaded)}, ${nw.triangles.toLocaleString()} triangles`);

// ---- instanced scene: the copy right in front of the camera streams its own cut
const mb = readFileSync(join(REPO, "web/assets/scene_rocks.bin"));
const mats = new Float32Array(mb.buffer.slice(mb.byteOffset, mb.byteOffset + mb.byteLength));
const rb = readFileSync(join(REPO, "web/assets/rock_paged.vgeow"));
const rlo = [0, 4, 8].map((o) => rb.readFloatLE(48 + o)), rhi = [0, 4, 8].map((o) => rb.readFloatLE(60 + o));
const rr = Math.hypot(...rhi.map((v, k) => v - rlo[k])) / 2;
const m0 = mats.slice(0, 16), s0 = Math.hypot(m0[0], m0[1], m0[2]);
const rc = [0, 1, 2].map((k) => (rlo[k] + rhi[k]) / 2);
const p0 = [0, 1, 2].map((r) => m0[r] * rc[0] + m0[4 + r] * rc[1] + m0[8 + r] * rc[2] + m0[12 + r]);
const nearView = { target: p0, distance: rr * s0 * 2.2, yaw: 0.3, pitch: 0.3 };
const results = {};
for (const [label, src, opts] of [["v1, no streamed copies", "../../web/assets/scene_instances.json", { streamedCopies: 0 }],
  ["v1, streamed copies", "../../web/assets/scene_instances.json", {}],
  ["paged, streamed copies", "../../web/assets/scene_instances_paged.json", {}]]) {
  const pg = await open(8781, src, { ...opts, ...nearView });
  results[label] = await settle(pg);
  await pg.screenshot({ path: join(OUT, `instances_${label.replace(/[^a-z]+/g, "_")}.png`) });
  await pg.close();
}
const a = results["v1, no streamed copies"], b = results["v1, streamed copies"], c = results["paged, streamed copies"];
check("nearest copy streams its own cut", b.streamedCopies >= 1 && a.streamedCopies === 0, `${b.streamedCopies} streamed copies`);
check("streamed copies draw fewer triangles than level 0", b.triangles < a.triangles,
  `${b.triangles.toLocaleString()} vs ${a.triangles.toLocaleString()} with whole-asset levels`);
check("paged instanced scene settles to the same cut", c.triangles === b.triangles && c.streamedCopies === b.streamedCopies,
  `${c.triangles.toLocaleString()} triangles, ${c.pagesLoaded}/${c.pagesTotal} pages`);

// ---- older scenes and files keep working
const old = await open(8781, "../../web/assets/scene_test.json", {});
const os_ = await settle(old);
await old.close();
check("existing scene JSON (v1 files) still loads", os_.triangles > 0 && os_.instances > 0, `${os_.triangles.toLocaleString()} triangles`);

await browser.close();
ranged.server.close();
plain.server.close();
console.log(`\n${failed ? "FAILED " + failed : "all passed"}  (screenshots in ${OUT})`);
process.exit(failed ? 1 : 0);
