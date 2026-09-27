"""GUI micro-benchmark: cost of chunk updates in the frames that follow.

    blender scene.blend --python tests/swap_bench.py -- [--shading SOLID|MATERIAL]

Uses the proxy's own chunk pairs (front at scale 1, back at scale 0).
"""
import os
import sys
import time

import bpy

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "addons"))
import vgeo  # noqa: E402
from vgeo import stream  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
SHADING = argv[argv.index("--shading") + 1] if "--shading" in argv else "SOLID"

frames = []
plan = []
results = []


def _draw():
    frames.append(time.perf_counter())


def area3d():
    for win in bpy.context.window_manager.windows:
        for area in win.screen.areas:
            if area.type == 'VIEW_3D':
                return area


def setup():
    vgeo.register()
    if bpy.app.timers.is_registered(stream._tick):
        bpy.app.timers.unregister(stream._tick)  # everything is driven by hand
    proxy = next(o for o in bpy.data.objects if o.vgeo.uid)
    area = area3d()
    area.spaces.active.overlay.show_overlays = False
    area.spaces.active.shading.type = SHADING
    bpy.types.SpaceView3D.draw_handler_add(_draw, (), 'WINDOW', 'POST_PIXEL')
    rt = stream.runtime_for(proxy)
    pairs = stream.chunk_objects(proxy, rt)
    stream.clear_backs(proxy, rt)
    by_size = sorted(range(len(pairs)), key=lambda c: len(stream.front_back(pairs[c])[0].data.polygons))
    top = by_size[-70:]
    mats = tuple(proxy.data.materials)
    print("shading", SHADING, "chunks", len(pairs), flush=True)

    def refill_backs(level, n):
        def f():
            rt.asset.select_level(level)
            tris = 0
            for c in top[:n]:
                back = stream.front_back(pairs[c])[1]
                stream.fill_mesh(back.data, rt.asset.extract(c), mats, False)
                tris += len(back.data.polygons)
            return f"{tris:,} tris"
        return f

    def flip():
        for c in top:
            front, back = stream.front_back(pairs[c])
            back.scale, front.scale = (1, 1, 1), (0, 0, 0)

    def no_shadow_backs(flag):
        def f():
            for c in top:
                stream.front_back(pairs[c])[1].visible_shadow = flag
        return f

    def swap_data(level):
        """The old way: fill new meshes and assign them."""
        def f():
            rt.asset.select_level(level)
            prepared = []
            for c in top:
                me = bpy.data.meshes.new("tmp")
                stream.fill_mesh(me, rt.asset.extract(c), mats, False)
                prepared.append((stream.front_back(pairs[c])[0], me))
            for ob, me in prepared:
                ob.data = me
        return f

    nothing = (lambda: None)
    if "--steady" in argv:
        # streaming-like load: a few backs refilled per frame, for many frames
        def steady(label, shadow, display='TEXTURED'):
            state = {"i": 0}

            def per_frame():
                c = top[state["i"] % len(top)]
                back = stream.front_back(pairs[c])[1]
                back.visible_shadow = shadow
                if back.display_type != display:
                    back.display_type = display
                rt.asset.select_level(1 + state["i"] % 2)
                stream.fill_mesh(back.data, rt.asset.extract(c), mats, False)
                state["i"] += 1
            return (label, per_frame)
        def direct():
            # chunks linked into the scene instead of instanced by the proxy's geometry nodes
            bpy.context.scene.collection.children.link(proxy.vgeo.collection)
            for m in proxy.modifiers:
                m.show_viewport = False
            return ("steady refill, shadow off, DIRECT (no GN instancing)", steady("", False)[1])

        def reveal():
            # all backs at once: BOUNDS -> TEXTURED (the moment before a flip)
            def f():
                t0 = time.perf_counter()
                for c in top:
                    stream.front_back(pairs[c])[1].display_type = 'TEXTURED'
                return f"reveal 70 backs"
            return f

        def unlinked(label, update):
            state = {"i": 0, "me": bpy.data.meshes.new("unlinked-test")}

            def per_frame():
                c = top[state["i"] % len(top)]
                rt.asset.select_level(1 + state["i"] % 2)
                d = rt.asset.extract(c)
                me = state["me"]
                if update:
                    stream.fill_mesh(me, d, mats, False)
                else:
                    raw_fill(me, d)
                state["i"] += 1
            return (label, per_frame)

        def raw_fill(me, d):
            # topology + positions only, via direct copies: no RNA update calls at all
            me.clear_geometry()
            nt = d["tri_count"]
            d["loop_starts"] = __import__("numpy").arange(0, nt * 3, 3, dtype="int32")
            me.vertices.add(d["vertex_count"])
            me.edges.add(d["edge_count"])
            me.loops.add(nt * 3)
            me.polygons.add(nt)
            stream._write_fast(me, d)

        from mathutils import Matrix

        def moving(label, inner):
            """Same work, while the view orbits every frame (like navigation)."""
            rv3d = area.spaces.active.region_3d
            base = rv3d.view_matrix.copy()
            state = {"i": 0}

            def per_frame():
                state["i"] += 1
                rv3d.view_matrix = base @ Matrix.Rotation(0.004 * state["i"], 4, 'Z')
                if inner:
                    inner()
            return (label, per_frame)

        STEADY.extend([moving("MOVING view, no work", None),
                       moving("MOVING view + fill UNLINKED (fill_mesh)", unlinked("", True)[1]),
                       moving("MOVING view + fill UNLINKED (no update calls)", unlinked("", False)[1]),
                       unlinked("fill UNLINKED mesh (fill_mesh)", True)])
        if "--direct" in argv:
            STEADY.append(("__direct__", direct))
        plan.extend([("reveal 70 filled backs (BOUNDS -> TEXTURED)", reveal()), ("idle", nothing),
                     ("flip 70", flip), ("idle", nothing)])
        bpy.app.timers.register(run_steady, first_interval=2.0)
        return
    plan.extend([
        ("idle", nothing),
        ("refill 10 hidden backs", refill_backs(1, 10)),
        ("idle", nothing),
        ("refill 70 hidden backs", refill_backs(1, 70)),
        ("idle", nothing),
        ("flip 70 by scale", flip),
        ("idle", nothing),
        ("backs: no shadow", no_shadow_backs(False)),
        ("idle", nothing),
        ("refill 10 hidden backs (no shadow)", refill_backs(2, 10)),
        ("refill 70 hidden backs (no shadow)", refill_backs(2, 70)),
        ("idle", nothing),
        ("flip 70 by scale", flip),
        ("idle", nothing),
        ("assign 70 new meshes (old way)", swap_data(3)),
        ("idle", nothing),
    ])
    bpy.app.timers.register(step, first_interval=2.0)


def step():
    area = area3d()
    if not plan:
        for r in results:
            print("SWAP", r, flush=True)
        bpy.ops.wm.quit_blender()
        return None
    name, fn = plan.pop(0)
    n0 = len(frames)
    t0 = time.perf_counter()
    info = fn()
    t1 = time.perf_counter()
    area.tag_redraw()

    def measure():
        if len(frames) <= n0 + 2:
            area.tag_redraw()
            return 0.005
        results.append(f"{name}: python {1000 * (t1 - t0):.0f} ms, next frame {1000 * (frames[n0] - t1):.1f} ms, "
                       f"then {1000 * (frames[n0 + 1] - frames[n0]):.1f} / {1000 * (frames[n0 + 2] - frames[n0 + 1]):.1f} ms"
                       + (f"  [{info}]" if info else ""))
        bpy.app.timers.register(step, first_interval=0.7)
        return None
    bpy.app.timers.register(measure, first_interval=0.0)
    return None


STEADY = []
_steady = {}


def run_steady():
    """Per phase: 2 s idle, then 3 s with one back refilled per timer tick."""
    now = time.perf_counter()
    if not _steady:
        if not STEADY:
            if plan:  # discrete steps queued after the steady phases
                bpy.app.timers.register(step, first_interval=0.5)
                return None
            for r in results:
                print("STEADY", r, flush=True)
            bpy.ops.wm.quit_blender()
            return None
        label, fn = STEADY.pop(0)
        if label == "__direct__":
            label, fn = fn()
        _steady.update(label=label, fn=fn, t0=now, n0=len(frames), ticks=0)
    area3d().tag_redraw()
    t = now - _steady["t0"]
    if t < 2.0:
        _steady["idle_n"] = len(frames)
        _steady["idle_t"] = now
        return 0.0
    if t < 5.0:
        _steady["fn"]()
        _steady["ticks"] += 1
        return 0.0
    fr = frames[_steady["idle_n"]:]
    dts = sorted(b - a for a, b in zip(fr, fr[1:]))
    idle = frames[_steady["n0"]:_steady["idle_n"]]
    idts = sorted(b - a for a, b in zip(idle, idle[1:]))
    results.append(f"{_steady['label']}: idle median {1000 * idts[len(idts) // 2]:.1f} ms | loaded median "
                   f"{1000 * dts[len(dts) // 2]:.1f} ms, p95 {1000 * dts[int(len(dts) * .95)]:.1f} ms, "
                   f"{_steady['ticks']} refills in {len(fr)} frames")
    _steady.clear()
    return 0.5


bpy.app.timers.register(setup, first_interval=1.0)
