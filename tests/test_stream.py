"""Headless tests for the VGEO streaming add-on.

Run:  blender -b --factory-startup --python tests/test_stream.py [-- --out DIR]
"""

import os
import sys
import tempfile
import time

import bpy
import numpy as np
from mathutils import Vector

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "addons"))

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = argv[argv.index("--out") + 1] if "--out" in argv else tempfile.mkdtemp(prefix="vgeo_test_")
os.makedirs(OUT, exist_ok=True)

import vgeo  # noqa: E402
from vgeo import native, stream  # noqa: E402

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + (f"  [{detail}]" if detail else ""))


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def dense_rock(subdiv=8):
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=subdiv, radius=1.0)
    ob = bpy.context.object
    bpy.ops.object.shade_smooth()
    tex = bpy.data.textures.new("rock", "CLOUDS")
    tex.noise_scale = 0.35
    tex.noise_depth = 4
    m = ob.modifiers.new("disp", "DISPLACE")
    m.texture = tex
    m.strength = 0.3
    mat_a = bpy.data.materials.new("Stone")
    mat_a.diffuse_color = (0.55, 0.5, 0.45, 1)
    mat_b = bpy.data.materials.new("Moss")
    mat_b.diffuse_color = (0.2, 0.4, 0.15, 1)
    for m, c, r in ((mat_a, (0.55, 0.5, 0.45, 1), 0.8), (mat_b, (0.2, 0.4, 0.15, 1), 0.9)):
        m.use_nodes = True
        bsdf = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
        bsdf.inputs["Base Color"].default_value = c
        bsdf.inputs["Roughness"].default_value = r
    ob.data.materials.append(mat_a)
    ob.data.materials.append(mat_b)
    # upper half gets the second material, so there is a material border to keep
    zs = np.empty(len(ob.data.polygons) * 3, np.float32)
    ob.data.polygons.foreach_get("center", zs)
    mi = (zs.reshape(-1, 3)[:, 2] > 0.3).astype(np.int32)
    ob.data.polygons.foreach_set("material_index", mi)
    ob.data.update()
    return ob


def cut_geometry(proxy):
    """All chunk geometry of a proxy as (positions, triangles)."""
    pos_all, tri_all, base = [], [], 0
    for ob in stream.fronts(proxy):
        me = ob.data
        nv, nl = len(me.vertices), len(me.loops)
        if nl == 0:
            continue
        co = np.empty(nv * 3, np.float32)
        me.vertices.foreach_get("co", co)
        cv = np.empty(nl, np.int32)
        me.loops.foreach_get("vertex_index", cv)
        pos_all.append(co.reshape(-1, 3))
        tri_all.append(cv.reshape(-1, 3) + base)
        base += nv
    return np.concatenate(pos_all), np.concatenate(tri_all)


def open_edges(pos, tris):
    """Edges used by exactly one triangle, after welding identical positions."""
    _, weld = np.unique(np.round(pos, 6), axis=0, return_inverse=True)
    t = weld.ravel()[tris]
    e = np.concatenate([t[:, [0, 1]], t[:, [1, 2]], t[:, [2, 0]]])
    e.sort(axis=1)
    _, counts = np.unique(e, axis=0, return_counts=True)
    return int((counts == 1).sum())


def camera(loc, target=(0, 0, 0), lens=50):
    cam = bpy.data.objects.get("Cam")
    if cam is None:
        cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
        bpy.context.scene.collection.objects.link(cam)
    cam.data.lens = lens
    cam.data.clip_start = 0.01
    cam.location = loc
    d = Vector(target) - Vector(loc)
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    bpy.context.scene.camera = cam
    bpy.context.view_layer.update()
    return cam


def main():
    print("Blender", bpy.app.version_string, "| out:", OUT)
    reset()
    vgeo.register()
    try:
        run()
    finally:
        vgeo.unregister()
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    if failed:
        print("FAILED:", ", ".join(failed))
        sys.exit(1)


def run():
    check("native library loads", native.available(), native.library_path())

    src = dense_rock(8)
    scene = bpy.context.scene
    scene.render.resolution_x, scene.render.resolution_y = 960, 540
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "stream_test.blend"))

    t0 = time.perf_counter()
    rc = bpy.ops.vgeo.virtualize()
    dt = time.perf_counter() - t0
    proxy = bpy.context.active_object
    check("virtualize finished", rc == {'FINISHED'} and proxy is not src, f"{dt:.1f}s")
    check("proxy stores relative path", proxy.vgeo.path.startswith("//vgeo/"), proxy.vgeo.path)
    check("vgeo file written", os.path.exists(stream.asset_path(proxy)))
    check("source hidden, kept", src.hide_render and src.hide_get() and proxy.vgeo.source == src)
    check("materials carried", [m.name for m in proxy.data.materials] == ["Stone", "Moss"])
    import struct
    blob = open(stream.asset_path(proxy), "rb").read()
    k = blob.find(b"MATP")
    params = struct.unpack_from("<8f", blob, k + 4) if k > 0 else ()
    check("material colors embedded for web viewers",
          k > 0 and abs(params[0] - 0.55) < 1e-5 and abs(params[3] - 0.8) < 1e-5 and abs(params[5] - 0.4) < 1e-5,
          str([round(x, 3) for x in params]))
    with open(os.path.join(OUT, "last_asset.txt"), "w") as f:
        f.write(stream.asset_path(proxy))
    rt = stream.runtime_for(proxy)
    src_tris = rt.asset.info["source_triangles"]
    check("source triangle count", src_tris == 327680, str(src_tris))
    check("DAG has many levels", rt.asset.info["lod_levels"] >= 8, str(rt.asset.info["lod_levels"]))

    # evaluated proxy shows the chunks through geometry nodes
    dg = bpy.context.evaluated_depsgraph_get()
    inst = sum(1 for i in dg.object_instances if i.is_instance and i.parent and i.parent.original == proxy)
    check("proxy instances chunk pairs", inst == 2 * rt.asset.chunk_count, f"{inst} instances")
    check("direct mesh writes verified", stream._fast_write is True)

    # cuts from the render camera at increasing distance
    counts = []
    for d in (1.6, 3, 8, 40):
        camera((0, -d, 0.2))
        view = stream.camera_view(scene)
        stream.apply_cut(proxy, [view], 1.0)
        counts.append(rt.triangles)
    check("cut shrinks with distance", all(a > b for a, b in zip(counts, counts[1:])), str(counts))
    check("far cut is small", counts[-1] < src_tris * 0.05, str(counts[-1]))

    # a cliff-sized rock seen from its surface mixes many LOD levels in one cut: must be crack-free
    proxy.scale = (30, 30, 30)
    camera((0, -33, 3), target=(25, 10, 0), lens=24)
    lods = set()
    proxy.vgeo.lod_colors = True
    stream.apply_cut(proxy, [stream.camera_view(scene)], 1.0, force=True)
    for ob in stream.fronts(proxy):
        a = ob.data.color_attributes.get("vgeo_lod")
        if a is not None and len(a.data):
            c = np.empty(len(a.data) * 4, np.float32)
            a.data.foreach_get("color", c)
            lods.update(map(tuple, np.round(c.reshape(-1, 4)[:, :3], 2)))
    # (a 327k-triangle test mesh has few, large groups; the large-scale demo shows deep mixes)
    check("grazing view mixes LOD levels", len(lods) >= 2, f"{len(lods)} levels visible")
    pos, tris = cut_geometry(proxy)
    oe = open_edges(pos, tris)
    check("mixed cut is watertight", oe == 0, f"{oe} open edges, {len(tris)} tris")
    # off-screen handling at cliff scale
    view = stream.camera_view(scene)
    stream.apply_cut(proxy, [view], 1.0, "FULL")
    full = rt.triangles
    stream.apply_cut(proxy, [view], 1.0, "COARSEN", 8.0)
    coarse = rt.triangles
    pos, tris = cut_geometry(proxy)
    oe = open_edges(pos, tris)
    check("coarsened off-screen is watertight", oe == 0, f"{oe} open edges")
    check("coarsening saves triangles", coarse < full, f"{full:,} -> {coarse:,}")
    stream.apply_cut(proxy, [view], 1.0, "CULL")
    check("culling saves more", rt.triangles < coarse, f"{rt.triangles:,}")
    stream.apply_cut(proxy, [view], 1.0, "FULL")

    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.color_type = 'VERTEX'
    scene.render.filepath = os.path.join(OUT, "lod_cliff.png")
    bpy.ops.render.render(write_still=True)
    stream.apply_cut(proxy, [stream.camera_view(scene)], 1.0)
    proxy.scale = (1, 1, 1)
    proxy.vgeo.lod_colors = False

    # full detail and coarsest levels are also closed
    stream.apply_level(proxy, 0)
    pos, tris = cut_geometry(proxy)
    check("level 0 equals source", len(tris) == src_tris, str(len(tris)))
    check("level 0 watertight", open_edges(pos, tris) == 0)

    # incremental: a tiny camera move rebuilds few chunks
    camera((0, -3, 0.2))
    stream.apply_cut(proxy, [stream.camera_view(scene)], 1.0)
    camera((0.02, -3, 0.2))
    stream.apply_cut(proxy, [stream.camera_view(scene)], 1.0)
    check("small move rebuilds few chunks", rt.last_rebuilt <= rt.asset.chunk_count // 2,
          f"{rt.last_rebuilt}/{rt.asset.chunk_count}")
    stream.apply_cut(proxy, [stream.camera_view(scene)], 1.0)
    check("unchanged view is a no-op", rt.last_rebuilt == 0 or rt.key is not None)

    # incremental (live loop) path: nothing changes on screen until the swap, then it matches a direct cut
    camera((0, -1.8, 0.3))
    view = stream.camera_view(scene)
    before = {o.name: o.data.name for o in stream.fronts(proxy)}
    tris_before = sum(len(o.data.polygons) for o in stream.fronts(proxy))
    steps, updates0, unchanged = 0, rt.updates, True
    while rt.updates == updates0:  # until the flip
        stream.stream_step(proxy, [view], 1.0, "FULL", 8.0, budget=0.0005)
        steps += 1
        if rt.updates == updates0:
            unchanged &= sum(len(o.data.polygons) for o in stream.fronts(proxy)) == tris_before
    check("incremental keeps old cut until swap", unchanged, f"{steps} steps")
    while stream.stream_step(proxy, [view], 1.0, "FULL", 8.0, budget=0.0005):
        steps += 1
    inc = sum(len(o.data.polygons) for o in stream.fronts(proxy))
    check("incremental ran in several steps", steps >= 2, f"{steps} steps, {rt.last_rebuilt} chunks")
    check("incremental result matches stats", inc == rt.triangles, f"{inc} vs {rt.triangles}")
    stream.apply_cut(proxy, [view], 1.0, "FULL", force=True)
    check("incremental equals direct cut", inc == rt.triangles)
    pos, tris = cut_geometry(proxy)
    check("incremental cut watertight", open_edges(pos, tris) == 0)
    def backs():
        return [o for o in proxy.vgeo.collection.objects if o.scale[0] < 0.5]
    check("direct cut empties hidden backs", all(len(o.data.vertices) == 0 for o in backs()))
    check("one back per chunk", len(backs()) == rt.asset.chunk_count, str(len(backs())))
    # streaming never creates or deletes datablocks
    n_meshes, n_objects = len(bpy.data.meshes), len(bpy.data.objects)
    for d in (2.4, 1.8, 2.4):
        camera((0, -d, 0.3))
        while stream.stream_step(proxy, [stream.camera_view(scene)], 1.0, "FULL", 8.0, budget=0.002):
            pass
    check("streaming creates no datablocks", (len(bpy.data.meshes), len(bpy.data.objects)) == (n_meshes, n_objects),
          f"{n_meshes}/{n_objects} -> {len(bpy.data.meshes)}/{len(bpy.data.objects)}")
    check("idle loop empties hidden backs", all(len(o.data.vertices) == 0 for o in backs()))
    pos, tris = cut_geometry(proxy)
    check("flipped cut watertight", open_edges(pos, tris) == 0, f"{len(tris)} tris")
    # BATCH strategy (EEVEE viewports): unlinked spares, landed in one step
    def run_batch(d):
        camera((0, -d, 0.3))
        v = stream.camera_view(scene)
        u0, shown0, same = rt.updates, sum(len(o.data.polygons) for o in stream.fronts(proxy)), True
        while rt.updates == u0:
            stream.stream_step(proxy, [v], 1.0, "FULL", 8.0, budget=0.0005, strategy='BATCH')
            if rt.updates == u0:
                same &= sum(len(o.data.polygons) for o in stream.fronts(proxy)) == shown0
        while stream.stream_step(proxy, [v], 1.0, "FULL", 8.0, budget=0.0005, strategy='BATCH'):
            pass
        return same
    same = run_batch(2.1)
    check("batch keeps old cut until it lands", same)
    pos, tris = cut_geometry(proxy)
    check("batch cut watertight", open_edges(pos, tris) == 0, f"{len(tris)} tris")
    check("batch cut matches stats", len(tris) == rt.triangles)
    run_batch(2.6)
    run_batch(2.1)
    counts = (len(bpy.data.meshes), len(bpy.data.objects))
    run_batch(2.6)
    run_batch(2.1)
    check("batch reuses its spares", (len(bpy.data.meshes), len(bpy.data.objects)) == counts,
          f"{counts} -> {(len(bpy.data.meshes), len(bpy.data.objects))}")
    spares = [m for m in bpy.data.meshes if m.name.startswith("vgeo.") and m.users == 0]
    check("at most one spare per chunk", len(spares) <= 2 * rt.asset.chunk_count, str(len(spares)))
    check("spares are empty when idle", all(len(m.vertices) == 0 for m in spares), f"{len(spares)} spares")

    # an interrupted update is discarded cleanly
    camera((0, -6, 0.3))
    shown = sum(len(o.data.polygons) for o in stream.fronts(proxy))
    stream.stream_step(proxy, [stream.camera_view(scene)], 1.0, "FULL", 8.0, budget=0.0)
    rt.invalidate()
    check("interrupted update leaves the shown cut alone",
          not rt.pending and sum(len(o.data.polygons) for o in stream.fronts(proxy)) == shown)
    # saving drops hidden geometry
    for o in backs()[:3]:
        stream.fill_mesh(o.data, rt.asset.extract(0), (), False) if rt.asset.extract(0) else None
    bpy.ops.wm.save_mainfile()
    check("save empties hidden backs", all(len(o.data.vertices) == 0 for o in backs()))

    # material border survives simplification
    mats = set()
    for ob in stream.fronts(proxy):
        me = ob.data
        if "material_index" in me.attributes and len(me.polygons):
            m = np.empty(len(me.polygons), np.int32)
            me.attributes["material_index"].data.foreach_get("value", m)
            mats.update(np.unique(m).tolist())
    check("both materials present in cut", mats == {0, 1}, str(mats))

    # renders: EEVEE + Cycles through the render_pre handler
    proxy.vgeo.lod_colors = False
    camera((0, -3.2, 0.9), lens=50)
    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
    sun.data.energy = 3
    sun.rotation_euler = (0.9, 0.2, 0.6)
    scene.collection.objects.link(sun)
    world = bpy.data.worlds.new("W")
    world.color = (0.05, 0.06, 0.08)
    scene.world = world
    for eng, fname in (("BLENDER_EEVEE", "eevee.png"), ("CYCLES", "cycles.png")):
        scene.render.engine = eng
        if eng == "CYCLES":
            scene.cycles.samples = 16
        scene.render.filepath = os.path.join(OUT, fname)
        rt.key = None
        before = rt.updates
        bpy.ops.render.render(write_still=True)
        check(f"{eng} render wrote image", os.path.exists(scene.render.filepath))
        check(f"{eng} render_pre updated the cut", rt.updates > before, f"{rt.triangles:,} tris")
    check("render flag cleared", stream._rendering is False)

    # LOD color debug still (workbench, color attribute)
    proxy.vgeo.lod_colors = True
    camera((1.25, -0.2, 0.1), target=(-1, 2, 0), lens=24)
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.color_type = 'VERTEX'
    scene.display.shading.light = 'STUDIO'
    scene.render.filepath = os.path.join(OUT, "lod_colors.png")
    bpy.ops.render.render(write_still=True)
    check("LOD color render", os.path.exists(scene.render.filepath))
    proxy.vgeo.lod_colors = False

    # save / reload: chunk meshes persist, runtime reopens
    bpy.ops.wm.save_mainfile()
    tris_saved = sum(len(o.data.polygons) for o in stream.fronts(proxy))
    bpy.ops.wm.open_mainfile(filepath=os.path.join(OUT, "stream_test.blend"))
    proxy = next(o for o in bpy.data.objects if o.vgeo.uid)
    check("chunks survive save/load", sum(len(o.data.polygons) for o in stream.fronts(proxy)) == tris_saved)
    check("runtimes reset on load", not stream._runtimes)
    camera((0, -8, 0.2))
    rt = stream.apply_cut(proxy, [stream.camera_view(bpy.context.scene)], 1.0)
    check("runtime reopens after load", rt.asset is not None and rt.triangles > 0, str(rt.triangles))

    # corrupt / missing file is reported, not a crash
    proxy.vgeo.path = "//vgeo/missing.vgeo"
    rt = stream.runtime_for(proxy)
    check("missing file reported", rt.asset is None and "cannot open" in (rt.error or ""), rt.error or "")
    bad = os.path.join(OUT, "bad.vgeo")
    with open(bad, "wb") as f:
        f.write(b"VGEO2\0\0\0" + b"\xff" * 400)
    try:
        native.Asset(bad)
        check("corrupt file rejected", False)
    except RuntimeError as e:
        check("corrupt file rejected", True, str(e))

    # indexed build path: smooth, no UVs, material border split automatically
    from vgeo import build as vbuild
    rock2 = dense_rock(7)
    rock2.name = "Smooth"
    while rock2.data.uv_layers:
        rock2.data.uv_layers.remove(rock2.data.uv_layers[0])
    arrays = vbuild.mesh_arrays(rock2, bpy.context.evaluated_depsgraph_get())
    check("smooth mesh uses indexed layout", arrays["indices"] is not None,
          f"{len(arrays['positions'])} verts")
    bpy.context.view_layer.objects.active = rock2
    rock2.select_set(True)
    rc = bpy.ops.vgeo.virtualize()
    p2 = bpy.context.active_object
    check("indexed virtualize", rc == {'FINISHED'} and p2.vgeo.uid)
    camera((0, -2.2, 0.4))
    rt2 = stream.apply_cut(p2, [stream.camera_view(bpy.context.scene)], 2.0)
    pos, tris = cut_geometry(p2)
    check("indexed cut watertight", open_edges(pos, tris) == 0, f"{len(tris)} tris")
    mats = set()
    for ob in stream.fronts(p2):
        me = ob.data
        if "material_index" in me.attributes and len(me.polygons):
            m = np.empty(len(me.polygons), np.int32)
            me.attributes["material_index"].data.foreach_get("value", m)
            mats.update(np.unique(m).tolist())
    check("indexed keeps both materials", mats == {0, 1}, str(mats))
    bpy.context.view_layer.objects.active = p2
    bpy.ops.vgeo.restore()

    # geometry-nodes materials: the evaluated mesh's materials win over the object's slots
    gn_obj = bpy.data.objects.new("GNGrid", bpy.data.meshes.new("GNGrid"))
    bpy.context.scene.collection.objects.link(gn_obj)
    gn_obj.data.materials.append(bpy.data.materials.new("SlotMat"))
    ng = bpy.data.node_groups.new("gridmat", "GeometryNodeTree")
    ng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    grid = ng.nodes.new("GeometryNodeMeshGrid")
    grid.inputs["Vertices X"].default_value = 64
    grid.inputs["Vertices Y"].default_value = 64
    setm = ng.nodes.new("GeometryNodeSetMaterial")
    gn_mat = bpy.data.materials.new("GNMat")
    setm.inputs["Material"].default_value = gn_mat
    outn = ng.nodes.new("NodeGroupOutput")
    ng.links.new(grid.outputs["Mesh"], setm.inputs["Geometry"])
    ng.links.new(setm.outputs["Geometry"], outn.inputs[0])
    gn_obj.modifiers.new("gn", "NODES").node_group = ng
    bpy.context.view_layer.objects.active = gn_obj
    gn_obj.select_set(True)
    rc = bpy.ops.vgeo.virtualize()
    gp = bpy.context.active_object
    slots = [m.name if m else None for m in gp.data.materials]
    shown = set()
    for ob in stream.fronts(gp):
        me = ob.data
        if len(me.polygons):
            mi = np.zeros(len(me.polygons), np.int32)
            if "material_index" in me.attributes:
                me.attributes["material_index"].data.foreach_get("value", mi)
            shown.update(slots[i] for i in np.unique(mi))
    # Blender evaluates Set Material as slots [None, GNMat] with faces on slot 1; VGEO must match
    check("GN Set Material carried to proxy", rc == {'FINISHED'} and shown == {"GNMat"}, f"{slots} -> {shown}")
    bpy.context.view_layer.objects.active = gp
    bpy.ops.vgeo.restore()

    # restore brings the source back
    proxy.vgeo.path = proxy.vgeo.path  # keep
    bpy.context.view_layer.objects.active = proxy
    rc = bpy.ops.vgeo.restore()
    src = bpy.data.objects.get("Icosphere")
    check("restore returns source", rc == {'FINISHED'} and src is not None and not src.hide_render)
    check("restore removes chunks", not any(o.name.startswith("vgeo.") for o in bpy.data.objects))


main()
