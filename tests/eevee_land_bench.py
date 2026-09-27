"""GUI benchmark: what EEVEE pays when streamed chunks change (needs the UI, not -b).

    blender scene.blend --python tests/eevee_land_bench.py -- [--shading MATERIAL|SOLID] [--out report.json]

Measures, for the proxy's chunk pairs, in two setups:
  GN      chunks instanced by the proxy's Geometry Nodes modifier (Collection Info)
  DIRECT  chunk collection linked into the scene, parented to the proxy
the frame cost of: refilling one hidden back per tick (steady), landing 70
chunks by swapping mesh data, and landing 70 prefilled backs by a scale flip.
"""

import json
import os
import statistics
import sys
import time

import bpy

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "addons"))
import vgeo  # noqa: E402
from vgeo import stream  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
SHADING = argv[argv.index("--shading") + 1] if "--shading" in argv else "MATERIAL"
OUT = argv[argv.index("--out") + 1] if "--out" in argv else None
N = 70

frames = []
report = {"shading": SHADING, "blender": bpy.app.version_string}
ctx = {}


def _draw():
    frames.append(time.perf_counter())


def area3d():
    for win in bpy.context.window_manager.windows:
        for area in win.screen.areas:
            if area.type == 'VIEW_3D':
                return area


def ms(a, b):
    return round(1000 * (b - a), 1)


def setup():
    vgeo.register()
    if bpy.app.timers.is_registered(stream._tick):
        bpy.app.timers.unregister(stream._tick)
    proxy = next(o for o in bpy.data.objects if o.vgeo.uid)
    area = area3d()
    area.spaces.active.overlay.show_overlays = False
    area.spaces.active.shading.type = SHADING
    bpy.types.SpaceView3D.draw_handler_add(_draw, (), 'WINDOW', 'POST_PIXEL')
    rt = stream.runtime_for(proxy)
    pairs = stream.chunk_objects(proxy, rt)
    stream.clear_backs(proxy, rt)
    for a, b in pairs:
        stream._set_shown(stream.front_back((a, b))[1], False)
    by_size = sorted(range(len(pairs)), key=lambda c: len(stream.front_back(pairs[c])[0].data.polygons))
    ctx.update(proxy=proxy, rt=rt, pairs=pairs, top=by_size[-N:], mats=tuple(proxy.data.materials),
               area=area, level=1)
    report["chunks"] = len(pairs)
    report["top_tris"] = sum(len(stream.front_back(pairs[c])[0].data.polygons) for c in ctx["top"])
    print("LAND setup", report, flush=True)
    queue.extend(program("GN"))
    queue.append(("call", go_direct))
    queue.extend(program("DIRECT"))
    bpy.app.timers.register(run, first_interval=1.5)


def go_direct():
    proxy = ctx["proxy"]
    col = proxy.vgeo.collection
    if col.name not in bpy.context.scene.collection.children:
        bpy.context.scene.collection.children.link(col)
    for ob in col.objects:
        ob.parent = proxy
        ob.matrix_parent_inverse.identity()
    for m in proxy.modifiers:
        m.show_viewport = False


def next_level():
    ctx["level"] = 1 + (ctx["level"] % 3)
    ctx["rt"].asset.select_level(ctx["level"])


def fill_back(c):
    back = stream.front_back(ctx["pairs"][c])[1]
    stream.fill_mesh(back.data, ctx["rt"].asset.extract(c), ctx["mats"], False)
    back.data.loop_triangles[0]
    back.data.corner_normals[0]


def program(label):
    top = ctx["top"]
    st = {"i": 0}

    def steady_one():
        if st["i"] % len(top) == 0:
            next_level()
        fill_back(top[st["i"] % len(top)])
        st["i"] += 1

    def prefill_spares():
        next_level()
        ctx["spares"] = []
        for c in top:
            me = bpy.data.meshes.new("land-spare")
            stream._sync_materials(me, ctx["mats"])
            stream.fill_mesh(me, ctx["rt"].asset.extract(c), ctx["mats"], False)
            me.loop_triangles[0]
            me.corner_normals[0]
            ctx["spares"].append(me)

    def swap_data():
        for c, me in zip(top, ctx["spares"]):
            stream.front_back(ctx["pairs"][c])[0].data = me

    def prefill_backs():
        next_level()
        for c in top:
            fill_back(c)

    def flip():
        for c in top:
            front, back = stream.front_back(ctx["pairs"][c])
            stream._set_shown(back, True)
            stream._set_shown(front, False)

    def clear_backs():
        for c in top:
            back = stream.front_back(ctx["pairs"][c])[1]
            back.data.clear_geometry()

    return [
        ("idle", f"{label} idle", 1.5, None),
        ("steady", f"{label} refill one hidden back per tick", 3.0, steady_one),
        ("call", clear_backs),
        ("idle", f"{label} idle after refills", 1.5, None),
        ("call", prefill_spares),
        ("idle", f"{label} (settle)", 1.5, None),
        ("event", f"{label} land {N} by mesh data swap", swap_data),
        ("idle", f"{label} (settle)", 1.5, None),
        ("call", prefill_backs),
        ("idle", f"{label} (settle after back fill)", 2.0, None),
        ("event", f"{label} land {N} prefilled backs by flip", flip),
        ("idle", f"{label} (settle)", 1.0, None),
        ("call", clear_backs),
        ("idle", f"{label} (settle)", 1.0, None),
    ]


queue = []
cur = {}


def summarize(label, fr, extra=None):
    dts = sorted(ms(a, b) for a, b in zip(fr, fr[1:]))
    if not dts:
        return
    r = {"frames": len(fr), "median": statistics.median(dts), "p95": dts[int(len(dts) * 0.95)], "max": dts[-1]}
    if extra:
        r.update(extra)
    if "(settle" not in label:
        report[label] = r
    print("LAND", label, json.dumps(r), flush=True)


def run():
    area = ctx["area"]
    area.tag_redraw()
    now = time.perf_counter()
    if cur:
        kind = cur["kind"]
        if kind in ("idle", "steady"):
            if now - cur["t0"] < cur["dur"]:
                if kind == "steady":
                    t = time.perf_counter()
                    cur["fn"]()
                    cur["py"].append(ms(t, time.perf_counter()))
                return 0.0
            extra = {"python_ms_median": statistics.median(cur["py"])} if cur["py"] else None
            summarize(cur["label"], frames[cur["n0"]:], extra)
        elif kind == "event":
            if len(frames) < cur["n0"] + 4 and now - cur["t0"] < 5:
                return 0.0
            fr = frames[cur["n0"]:cur["n0"] + 4]
            r = {"python_ms": cur["py"], "next_frame_ms": ms(cur["t1"], fr[0]) if fr else None,
                 "following_ms": [ms(a, b) for a, b in zip(fr, fr[1:])]}
            report[cur["label"]] = r
            print("LAND", cur["label"], json.dumps(r), flush=True)
        cur.clear()
    if not queue:
        if OUT:
            with open(OUT, "w") as f:
                json.dump(report, f, indent=2)
        bpy.ops.wm.quit_blender()
        return None
    item = queue.pop(0)
    if item[0] == "call":
        item[1]()
        return 0.0
    kind, label = item[0], item[1]
    cur.update(kind=kind, label=label, n0=len(frames), t0=time.perf_counter(), py=[])
    if kind in ("idle", "steady"):
        cur.update(dur=item[2], fn=item[3])
    else:
        t = time.perf_counter()
        item[2]()
        cur["t1"] = time.perf_counter()
        cur["py"] = ms(t, cur["t1"])
        cur["n0"] = len(frames)
    return 0.0


bpy.app.timers.register(setup, first_interval=1.0)
