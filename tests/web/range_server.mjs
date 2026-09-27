// Static file server with HTTP range requests (python -m http.server has none), for testing
// paged .vgeow streaming. node tests/web/range_server.mjs [--port 8780] [--root DIR] [--no-range]
// GET /__stats returns {requests, bytes, ranges} served since start (or since /__reset).
import { createServer } from "node:http";
import { createReadStream, statSync } from "node:fs";
import { extname, join, normalize, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const TYPES = { ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript", ".json": "application/json",
  ".png": "image/png", ".wasm": "application/wasm" };

export function startServer({ port = 8780, root = process.cwd(), ranges = true } = {}) {
  const stats = { requests: 0, bytes: 0, ranges: 0 };
  const base = resolve(root);
  const server = createServer((req, res) => {
    const url = new URL(req.url, "http://x");
    if (url.pathname === "/__stats") { res.end(JSON.stringify(stats)); return; }
    if (url.pathname === "/__reset") { Object.assign(stats, { requests: 0, bytes: 0, ranges: 0 }); res.end("{}"); return; }
    const file = normalize(join(base, decodeURIComponent(url.pathname)));
    if (!file.startsWith(base)) { res.writeHead(403).end(); return; }
    let st;
    try { st = statSync(file); } catch { res.writeHead(404).end(); return; }
    if (st.isDirectory()) { res.writeHead(404).end(); return; }
    const type = TYPES[extname(file)] || "application/octet-stream";
    const m = ranges && /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || "");
    stats.requests++;
    if (m) {
      const start = Number(m[1]);
      const end = Math.min(m[2] ? Number(m[2]) : st.size - 1, st.size - 1);
      if (start > end) { res.writeHead(416, { "Content-Range": `bytes */${st.size}` }).end(); return; }
      stats.ranges++;
      stats.bytes += end - start + 1;
      res.writeHead(206, { "Content-Type": type, "Content-Length": end - start + 1, "Accept-Ranges": "bytes",
        "Content-Range": `bytes ${start}-${end}/${st.size}`, "Cache-Control": "no-store" });
      createReadStream(file, { start, end }).pipe(res);
    } else {
      stats.bytes += st.size;
      res.writeHead(200, { "Content-Type": type, "Content-Length": st.size, "Cache-Control": "no-store",
        ...(ranges ? { "Accept-Ranges": "bytes" } : {}) });
      createReadStream(file).pipe(res);
    }
  });
  return new Promise((ok) => server.listen(port, "127.0.0.1", () => ok({ server, stats })));
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const a = process.argv.slice(2);
  const port = a.includes("--port") ? Number(a[a.indexOf("--port") + 1]) : 8780;
  const root = a.includes("--root") ? a[a.indexOf("--root") + 1] : process.cwd();
  await startServer({ port, root, ranges: !a.includes("--no-range") });
  console.log(`serving ${root} on http://127.0.0.1:${port}/${a.includes("--no-range") ? " (no ranges)" : ""}`);
}
