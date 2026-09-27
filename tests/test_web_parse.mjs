// node tests/test_web_parse.mjs <file.vgeo>
// Checks the web viewer's parser against a file written by the native builder.
import { readFileSync } from "node:fs";
import { parseVGEO } from "../web/vgeo-viewer.js";

const file = process.argv[2];
const bytes = readFileSync(file);
const buf = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
const h = parseVGEO(buf);
let failed = 0;
const check = (name, ok, detail = "") => { console.log((ok ? "PASS " : "FAIL ") + name + (detail ? `  [${detail}]` : "")); if (!ok) failed++; };
check("header counts", h.clusterCount > 0 && h.vertexCount > 0 && h.indexCount % 3 === 0, `${h.clusterCount} clusters`);
check("sections inside the file", [h.offPositions, h.offIndices, h.offClusters, h.offGroups, h.offMaterials].every((o) => o > 0 && o < h.fileSize));
check("material names", h.materialNames.join(",") === "Stone,Moss", h.materialNames.join(","));
check("material params", h.materialParams && Math.abs(h.materialParams[1].color[1] - 0.4) < 1e-5 && Math.abs(h.materialParams[0].roughness - 0.8) < 1e-5,
  JSON.stringify(h.materialParams));
check("widest cluster", h.maxClusterTris > 0 && h.maxClusterTris <= 256, String(h.maxClusterTris));
// every cluster references a valid group and index range
const cl = new Uint32Array(buf, h.offClusters, h.clusterCount * 10);
let ok = true;
for (let i = 0; i < h.clusterCount && ok; i++) {
  const off = cl[i * 10], tc = cl[i * 10 + 1], g = cl[i * 10 + 2];
  ok = off + tc * 3 <= h.indexCount && g < h.groupCount;
}
check("cluster table consistent", ok);
let bad = false;
try { parseVGEO(new ArrayBuffer(300)); } catch { bad = true; }
check("rejects non-VGEO data", bad);
console.log(`\n${failed ? "FAILED " + failed : "all passed"}`);
process.exit(failed ? 1 : 0);
