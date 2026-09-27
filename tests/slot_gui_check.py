"""GUI check: a scattered copy seen up close switches to its own streamed cut (needs the UI).

    blender scatter_demo.blend --python tests/slot_gui_check.py -- --out DIR [--shading MATERIAL]

Points the viewport at the copy nearest the scene camera from close range,
lets the live loop run, and records: when the copy's streamed cut took over,
triangles before/after, frame times while it streamed, and screenshots
before and after the switch. Prints SLOT lines and a JSON report.
"""

import json
import os
import sys
import time

import bpy
import numpy as np
from mathutils import Vector

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "addons"))
import vgeo  # noqa: E402
from vgeo import instances, stream  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = argv[argv.index("--out") + 1] if "--out" in argv else os.path.join(REPO, "build-demo", "slot_check")
SHADING = argv[argv.index("--shading") + 1] if "--shading" in argv else "MATERIAL"
os.makedirs(OUT, exist_ok=True)

st = {"frames": [], "t0": None, "report": {"shading": SHADING, "blender": bpy.app.version_string}}


def view3d():
    for win in bpy.context.window_manager.windows:
        for area in win.screen.areas:
            if area.type == 'VIEW_3D':
                return win, area, next(r for r in area.regions if r.type == 'WINDOW'), area.spaces.active


def shot(name):
    win, area, region, _space = view3d()
    with bpy.context.temp_override(window=win, area=area, region=region):
        bpy.ops.screen.screenshot_area(filepath=os.path.join(OUT, name))


def _draw():
    st["frames"].append(time.perf_counter())


def setup():
    vgeo.register()
    inst = instances.instancers()[0]
    st["inst"] = inst
    inst.vgeo_inst.stream_slots = 4
    win, area, region, space = view3d()
    space.overlay.show_overlays = False
    space.shading.type = SHADING
    space.clip_start = 0.01
    bpy.types.SpaceView3D.draw_handler_add(_draw, (), 'WINDOW', 'POST_PIXEL')
    # the copy nearest the scene camera, seen from 1.8 radii
    cam = bpy.context.scene.camera.matrix_world.translation
    me = inst.data
    co = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    world = np.array([tuple(inst.matrix_world @ Vector(c)) for c in co])
    i = int(np.argmin(np.linalg.norm(world - np.array(cam), axis=1)))
    sc = float(me.attributes["vgeo_scale"].data[i].value) * max(inst.matrix_world.to_scale())
    r = float(inst["vgeo_radius"]) * sc
    p = Vector(world[i])
    eye = p + (Vector(cam) - p).normalized() * r * 1.8
    rv3d = space.region_3d
    rv3d.view_perspective = 'PERSP'
    rv3d.view_location = p
    rv3d.view_distance = (eye - p).length
    rv3d.view_rotation = (p - eye).to_track_quat('-Z', 'Y')
    st.update(target=i, t0=time.perf_counter(), n0=len(st["frames"]))
    st["report"]["target"] = i
    st["report"]["radius"] = r
    bpy.app.timers.register(watch, first_interval=0.05)


def watch():
    inst = st["inst"]
    view3d()[1].tag_redraw()
    now = time.perf_counter() - st["t0"]
    flags = instances._streamed_flags(inst)
    rep = st["report"]
    if "before_png" not in rep and now > 0.3:
        shot("slot_before.png")
        rep["before_png"] = True
        rep["tris_before"] = inst.vgeo_inst.shown_triangles
    if flags.any() and "switched_s" not in rep:
        rep["switched_s"] = round(now, 2)
        rep["streamed_count"] = int(flags.sum())
        rep["target_streamed"] = bool(flags[st["target"]])
        st["t_switch"] = time.perf_counter()
    if "switched_s" in rep and time.perf_counter() - st["t_switch"] > 1.5:
        fr = st["frames"][st["n0"]:]
        dts = sorted(1000 * (b - a) for a, b in zip(fr, fr[1:]))
        rep["frame_ms_median"] = round(dts[len(dts) // 2], 1) if dts else 0
        rep["frame_ms_max"] = round(dts[-1], 1) if dts else 0
        rep["tris_after"] = inst.vgeo_inst.shown_triangles
        _e, tris = instances._level_tables(inst)
        rep["level0_tris"] = int(tris[0])
        rep["slot_tris"] = [stream._runtimes[o.vgeo.uid].triangles for o in instances.slots(inst)
                            if o.vgeo.slot_index >= 0 and o.vgeo.uid in stream._runtimes]
        shot("slot_after.png")
        finish()
        return None
    if now > 20:
        rep["timeout"] = True
        finish()
        return None
    return 0.02


def finish():
    print("SLOT", json.dumps(st["report"]), flush=True)
    with open(os.path.join(OUT, "slot_report.json"), "w") as f:
        json.dump(st["report"], f, indent=2)
    bpy.ops.wm.quit_blender()


bpy.app.timers.register(setup, first_interval=1.0)
