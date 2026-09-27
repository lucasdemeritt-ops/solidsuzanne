"""Streams a view-dependent cut of a .vgeo asset into real Blender meshes.

A virtualized object is a normal mesh object (the proxy) whose Geometry
Nodes modifier instances a hidden collection of chunk objects. Each chunk
holds the part of the current DAG cut that falls in its region of space.
When the view changes, the native runtime picks a new cut and only the
chunks whose contents changed are rebuilt, all within one tick so the
surface stays watertight. EEVEE, Cycles and Workbench render the chunks
like any other mesh: full materials, lights, shadows, ray tracing.
"""

import ctypes
import time
import uuid

import bpy
import numpy as np
from bpy.app.handlers import persistent
from mathutils import Vector

from . import native

NODE_GROUP = "VGEO Stream"
TICK = 0.1           # seconds between view checks when idle
TICK_BUDGET = 0.014  # seconds of mesh building per tick while an update is in flight

_runtimes = {}       # uid -> Runtime
_rendering = False

LOD_PALETTE = np.array([
    (0.90, 0.30, 0.25, 1), (0.95, 0.60, 0.20, 1), (0.95, 0.85, 0.25, 1), (0.55, 0.85, 0.30, 1),
    (0.25, 0.75, 0.55, 1), (0.25, 0.65, 0.90, 1), (0.40, 0.45, 0.95, 1), (0.65, 0.40, 0.90, 1),
    (0.90, 0.40, 0.75, 1), (0.70, 0.70, 0.70, 1), (0.45, 0.30, 0.20, 1), (0.20, 0.35, 0.30, 1),
], dtype=np.float32)


class Runtime:
    """Native asset handle plus what is currently written into the chunk meshes."""

    def __init__(self, uid, path):
        self.uid = uid
        self.path = path
        self.error = None
        self.asset = None
        try:
            self.asset = native.Asset(path)
        except Exception as e:  # missing file, corrupt file, no DLL
            self.error = str(e)
            return
        n = self.asset.chunk_count
        self.applied = np.zeros(n, dtype=np.uint64)
        self.valid = np.zeros(n, dtype=bool)
        self.key = None
        self.lod_colors = None
        self.triangles = 0
        self.clusters = 0
        self.last_ms = 0.0
        self.latency_ms = 0.0
        self.last_rebuilt = 0
        self.updates = 0
        # incremental update in flight (see stream_step)
        self.target = None
        self.todo = []
        self.pending = {}

    def invalidate(self):
        if self.asset:
            self.valid[:] = False
            self.key = None
            # drop a half-built update; removal by name only touches unused "...next" meshes,
            # so it is safe even after undo replaced the datablocks
            _discard_pending(self)

    def close(self):
        if self.asset:
            self.asset.close()
            self.asset = None


# ---------------------------------------------------------------- helpers

def proxies(scene=None):
    """Virtualized objects, optionally limited to one scene."""
    objs = scene.objects if scene is not None else bpy.data.objects
    return [o for o in objs if o.type == 'MESH' and o.vgeo.uid and o.vgeo.path]


def asset_path(obj):
    return bpy.path.abspath(obj.vgeo.path, library=obj.library)


def runtime_for(obj):
    uid = obj.vgeo.uid
    path = asset_path(obj)
    rt = _runtimes.get(uid)
    if rt is None or rt.path != path:
        if rt:
            rt.close()
        rt = Runtime(uid, path)
        _runtimes[uid] = rt
    return rt


def invalidate_all(close=False):
    for rt in list(_runtimes.values()):
        if close:
            rt.close()
        else:
            rt.invalidate()
    if close:
        _runtimes.clear()


def chunk_name(uid, i):
    return f"vgeo.{uid}.{i:04d}"


def ensure_node_group():
    ng = bpy.data.node_groups.get(NODE_GROUP)
    if ng and ng.bl_idname == "GeometryNodeTree" and "Chunks" in ng.interface.items_tree:
        return ng
    ng = bpy.data.node_groups.new(NODE_GROUP, "GeometryNodeTree")
    ng.interface.new_socket("Chunks", in_out="INPUT", socket_type="NodeSocketCollection")
    ng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    gin = ng.nodes.new("NodeGroupInput")
    gin.location = (-300, 0)
    info = ng.nodes.new("GeometryNodeCollectionInfo")
    info.transform_space = "ORIGINAL"
    info.inputs["Separate Children"].default_value = False
    info.inputs["Reset Children"].default_value = False
    out = ng.nodes.new("NodeGroupOutput")
    out.location = (300, 0)
    ng.links.new(gin.outputs["Chunks"], info.inputs["Collection"])
    ng.links.new(info.outputs["Instances"], out.inputs["Geometry"])
    return ng


def modifier_socket_id(ng):
    return ng.interface.items_tree["Chunks"].identifier


def chunk_objects(obj, rt):
    """Chunk objects by index, creating any that are missing (e.g. after a file was moved)."""
    col = obj.vgeo.collection
    if col is None:
        col = bpy.data.collections.new(f".vgeo {obj.vgeo.uid}")
        obj.vgeo.collection = col
        _attach_modifier(obj)
    out = []
    for i in range(rt.asset.chunk_count):
        name = chunk_name(obj.vgeo.uid, i)
        ob = bpy.data.objects.get(name)
        if ob is None or ob.type != 'MESH':
            me = bpy.data.meshes.new(name)
            ob = bpy.data.objects.new(name, me)
        if col not in ob.users_collection:
            col.objects.link(ob)
        out.append(ob)
    return out


def _attach_modifier(obj):
    ng = ensure_node_group()
    mod = next((m for m in obj.modifiers if m.type == 'NODES' and m.node_group == ng), None)
    if mod is None:
        mod = obj.modifiers.new("VGEO Stream", 'NODES')
        mod.node_group = ng
    mod[modifier_socket_id(ng)] = obj.vgeo.collection


# ---------------------------------------------------------------- views

def viewport_views():
    """(view_matrix, window_matrix, height_px, is_persp, clip_start) for every visible 3D view."""
    views = []
    wm = bpy.context.window_manager
    for win in wm.windows:
        screen = win.screen
        if screen is None:
            continue
        for area in screen.areas:
            if area.type != 'VIEW_3D':
                continue
            space = area.spaces.active
            regions = [r for r in area.regions if r.type == 'WINDOW' and r.width > 1 and r.height > 1]
            rv3ds = list(space.region_quadviews) if space.region_quadviews else [space.region_3d]
            for region, rv3d in zip(regions, rv3ds):
                if rv3d is None:
                    continue
                views.append((rv3d.view_matrix.copy(), rv3d.window_matrix.copy(), region.height,
                              rv3d.is_perspective, space.clip_start))
    return views


def camera_view(scene, depsgraph=None):
    """The render camera as a view tuple, at final render resolution."""
    cam = scene.camera
    if cam is None:
        return None
    r = scene.render
    w = max(1, int(r.resolution_x * r.resolution_percentage / 100))
    h = max(1, int(r.resolution_y * r.resolution_percentage / 100))
    dg = depsgraph or bpy.context.evaluated_depsgraph_get()
    cam_eval = cam.evaluated_get(dg)
    proj = cam_eval.calc_matrix_camera(dg, x=w, y=h, scale_x=r.pixel_aspect_x, scale_y=r.pixel_aspect_y)
    persp = cam.type == 'CAMERA' and cam.data.type != 'ORTHO'
    clip = cam.data.clip_start if cam.type == 'CAMERA' else 0.01
    return (cam_eval.matrix_world.inverted(), proj, h, persp, clip)


def _frustum_planes(clip):
    rows = [clip.row[i] for i in range(4)]
    planes = []
    for i in range(3):
        for s in (1, -1):
            p = Vector(rows[3]) + s * Vector(rows[i])
            n = p.xyz.length or 1.0
            planes.append((p.x / n, p.y / n, p.z / n, p.w / n))
    return planes


def local_views(obj, views, pixel_error, mode="FULL", offscreen_scale=8.0):
    mw = obj.matrix_world
    try:
        inv = mw.inverted()
    except ValueError:
        return []
    scale = max(abs(s) for s in mw.to_scale()) or 1.0
    out = []
    for view, window, height, persp, clip_start in views:
        cam_world = view.inverted().translation
        cam_local = inv @ cam_world
        threshold = pixel_error / max(1, height)
        planes = _frustum_planes(window @ view @ mw) if mode != "FULL" else None
        if persp:
            out.append(native.make_view(cam_local, window[1][1], max(clip_start / scale, 1e-6),
                                        threshold, planes=planes, mode=mode, offscreen_scale=offscreen_scale))
        else:
            ortho_h = 2.0 / window[1][1] if window[1][1] else 1.0
            out.append(native.make_view(cam_local, 1.0, 1e-6, threshold, ortho=True,
                                        ortho_height=ortho_h / scale, planes=planes, mode=mode,
                                        offscreen_scale=offscreen_scale))
    return out


def _key(obj, views, pixel_error, mode, offscreen_scale):
    parts = [round(pixel_error, 4), mode, round(offscreen_scale, 3), bool(obj.vgeo.lod_colors)]
    for m in [obj.matrix_world] + [v[0] for v in views] + [v[1] for v in views]:
        parts.extend(round(x, 5) for row in m for x in row)
    parts.extend(v[2] for v in views)
    return tuple(parts)


# ---------------------------------------------------------------- mesh writing

# Direct copies into Blender's attribute arrays are 5-10x faster than
# foreach_set, which takes a per-item path for topology arrays. The layout is
# verified once per session against foreach_get; any mismatch switches back
# to foreach_set for good.
_fast_write = None   # None = not verified yet


def _copy_into(attr_data, arr):
    ctypes.memmove(attr_data[0].as_pointer(), arr.ctypes.data, arr.nbytes)


def _write_fast(me, d):
    A = me.attributes
    _copy_into(A["position"].data, d["positions"])
    _copy_into(A[".edge_verts"].data, d["edge_verts"])
    _copy_into(A[".corner_vert"].data, d["corner_verts"])
    _copy_into(A[".corner_edge"].data, d["corner_edges"])
    ctypes.memmove(me.polygons[0].as_pointer(), d["loop_starts"].ctypes.data, d["loop_starts"].nbytes)


def _write_slow(me, d):
    me.vertices.foreach_set("co", d["positions"])
    me.edges.foreach_set("vertices", d["edge_verts"])
    me.loops.foreach_set("vertex_index", d["corner_verts"])
    me.loops.foreach_set("edge_index", d["corner_edges"])
    me.polygons.foreach_set("loop_start", d["loop_starts"])


def _verify(me, d):
    def get(seq, prop, n, dtype):
        out = np.empty(n, dtype)
        seq.foreach_get(prop, out)
        return out
    nt = d["tri_count"]
    ok = (np.array_equal(get(me.vertices, "co", d["vertex_count"] * 3, np.float32), d["positions"])
          and np.array_equal(get(me.edges, "vertices", d["edge_count"] * 2, np.int32), d["edge_verts"])
          and np.array_equal(get(me.loops, "vertex_index", nt * 3, np.int32), d["corner_verts"])
          and np.array_equal(get(me.loops, "edge_index", nt * 3, np.int32), d["corner_edges"])
          and np.array_equal(get(me.polygons, "loop_start", nt, np.int32), d["loop_starts"])
          and np.array_equal(get(me.polygons, "loop_total", nt, np.int32), np.full(nt, 3, np.int32)))
    return ok and not me.validate(verbose=False)


def _set_attr(me, name, kind, domain, prop, arr):
    a = me.attributes.new(name, kind, domain)
    if _fast_write:
        _copy_into(a.data, arr)
    else:
        a.data.foreach_set(prop, arr)
    return a


def fill_mesh(me, data, materials, lod_colors):
    """Replace a mesh's geometry with an extracted chunk."""
    global _fast_write
    me.clear_geometry()
    if data is None:
        return
    nv, nt, ne = data["vertex_count"], data["tri_count"], data["edge_count"]
    data["loop_starts"] = np.arange(0, nt * 3, 3, dtype=np.int32)
    me.vertices.add(nv)
    me.edges.add(ne)
    me.loops.add(nt * 3)
    me.polygons.add(nt)
    if _fast_write is None:
        try:
            _write_fast(me, data)
            _fast_write = _verify(me, data)
        except Exception:
            _fast_write = False
        if not _fast_write:
            print("VGEO: direct mesh writes unavailable, using foreach_set")
            me.clear_geometry()
            me.vertices.add(nv)
            me.edges.add(ne)
            me.loops.add(nt * 3)
            me.polygons.add(nt)
            _write_slow(me, data)
    elif _fast_write:
        _write_fast(me, data)
    else:
        _write_slow(me, data)

    _set_attr(me, "custom_normal", 'FLOAT_VECTOR', 'POINT', "vector", data["normals"])
    if data["uvs"] is not None:
        corner_uv = np.ascontiguousarray(data["uvs"].reshape(-1, 2)[data["corner_verts"]]).ravel()
        _set_attr(me, "UVMap", 'FLOAT2', 'CORNER', "vector", corner_uv)
    if len(materials) > 1:
        _set_attr(me, "material_index", 'INT', 'FACE', "value", data["face_materials"])
    if lod_colors:
        lod = np.repeat(data["face_lod"] % len(LOD_PALETTE), 3)
        col = _set_attr(me, "vgeo_lod", 'FLOAT_COLOR', 'CORNER', "color",
                        np.ascontiguousarray(LOD_PALETTE[lod]).ravel())
        me.color_attributes.active_color = col
    me.update()


def _sync_materials(me, materials):
    if tuple(me.materials) != materials:
        me.materials.clear()
        for m in materials:
            me.materials.append(m)


def _discard_pending(rt):
    """Drop a half-built incremental update (its meshes were never shown)."""
    for name in rt.pending.values():
        me = bpy.data.meshes.get(name)
        if me is not None and me.users == 0:
            bpy.data.meshes.remove(me)
    rt.pending.clear()
    rt.todo = []
    rt.target = None


def stream_step(obj, views, pixel_error, mode="COARSEN", offscreen_scale=8.0, budget=0.012):
    """Incremental update for the live loop; returns True while work remains.

    Replacement meshes for changed chunks are built a few at a time into new,
    unlinked datablocks while the current (valid) cut stays on screen. Once
    all are ready they are swapped in together, so the surface never shows a
    mix of two cuts. The native selection is not touched until the swap, so
    extraction keeps reading the target cut.
    """
    rt = runtime_for(obj)
    if rt.asset is None:
        return False
    if rt.target is None:
        key = _key(obj, views, pixel_error, mode, offscreen_scale)
        if key == rt.key and rt.valid.all():
            return False
        lv = local_views(obj, views, pixel_error, mode, offscreen_scale)
        if not lv:
            return False
        t0 = time.perf_counter()
        sigs = rt.asset.select(lv)
        lod_colors = bool(obj.vgeo.lod_colors)
        if rt.lod_colors != lod_colors:
            rt.valid[:] = False
            rt.lod_colors = lod_colors
        changed = np.nonzero(~rt.valid | (sigs != rt.applied))[0]
        rt.target_key = key
        rt.target_stats = (int(rt.asset.last.triangles), int(rt.asset.last.clusters))
        rt.target_t0 = t0
        if len(changed) == 0:
            rt.key = key
            rt.triangles, rt.clusters = rt.target_stats
            return False
        rt.target = sigs.copy()
        rt.todo = [int(c) for c in changed[::-1]]  # pop() takes them in order
        rt.build_s = 0.0

    t_start = time.perf_counter()
    materials = tuple(obj.data.materials)
    while rt.todo and time.perf_counter() - t_start < budget:
        c = rt.todo.pop()
        me = bpy.data.meshes.new(chunk_name(rt.uid, c) + ".next")
        _sync_materials(me, materials)
        fill_mesh(me, rt.asset.extract(c), materials, rt.lod_colors)
        rt.pending[c] = me.name
    rt.build_s += time.perf_counter() - t_start
    if rt.todo:
        return True

    # everything is built: swap all changed chunks in one go
    chunks = chunk_objects(obj, rt)
    for c, name in rt.pending.items():
        new = bpy.data.meshes.get(name)
        if new is None:  # lost to undo: start over next tick
            _discard_pending(rt)
            rt.invalidate()
            return True
        ob = chunks[c]
        old = ob.data
        ob.data = new
        if old is not None and old.users == 0:
            bpy.data.meshes.remove(old)
        new.name = chunk_name(rt.uid, c)
        rt.applied[c] = rt.target[c]
        rt.valid[c] = True
    rt.last_rebuilt = len(rt.pending)
    rt.pending.clear()
    rt.target = None
    rt.key = rt.target_key
    rt.triangles, rt.clusters = rt.target_stats
    rt.last_ms = rt.build_s * 1000.0
    rt.latency_ms = (time.perf_counter() - rt.target_t0) * 1000.0
    rt.updates += 1
    return False


def apply_cut(obj, views, pixel_error, mode="FULL", offscreen_scale=8.0, force=False):
    """Select a cut for these (world-space) views and write changed chunks now.

    Used for renders and scripts. mode: FULL, COARSEN (off-screen detail
    reduced by offscreen_scale) or CULL. Returns the Runtime (with stats).
    """
    rt = runtime_for(obj)
    if rt.asset is None:
        return rt
    _discard_pending(rt)
    key = _key(obj, views, pixel_error, mode, offscreen_scale)
    if not force and key == rt.key and rt.valid.all():
        return rt
    t0 = time.perf_counter()
    lv = local_views(obj, views, pixel_error, mode, offscreen_scale)
    if not lv:
        return rt
    sigs = rt.asset.select(lv)
    lod_colors = bool(obj.vgeo.lod_colors)
    if rt.lod_colors != lod_colors:
        rt.valid[:] = False
        rt.lod_colors = lod_colors
    changed = np.nonzero(~rt.valid | (sigs != rt.applied))[0]
    materials = tuple(obj.data.materials)
    chunks = chunk_objects(obj, rt) if len(changed) or not rt.valid.all() else None
    for c in changed:
        me = chunks[c].data
        _sync_materials(me, materials)
        fill_mesh(me, rt.asset.extract(int(c)), materials, lod_colors)
        rt.applied[c] = sigs[c]
        rt.valid[c] = True
    if chunks is not None and len(changed) == 0:
        for ob in chunks:
            _sync_materials(ob.data, materials)
    rt.key = key
    rt.triangles = int(rt.asset.last.triangles)
    rt.clusters = int(rt.asset.last.clusters)
    rt.last_rebuilt = int(len(changed))
    rt.last_ms = (time.perf_counter() - t0) * 1000.0
    rt.updates += 1
    return rt


def apply_level(obj, depth):
    """Write a fixed DAG level (0 = full detail, -1 = coarsest). For previews and tests."""
    rt = runtime_for(obj)
    if rt.asset is None:
        return rt
    _discard_pending(rt)
    sigs = rt.asset.select_level(depth)
    chunks = chunk_objects(obj, rt)
    materials = tuple(obj.data.materials)
    for c in range(rt.asset.chunk_count):
        _sync_materials(chunks[c].data, materials)
        fill_mesh(chunks[c].data, rt.asset.extract(c), materials, bool(obj.vgeo.lod_colors))
        rt.applied[c] = sigs[c]
    rt.valid[:] = True
    rt.key = None
    rt.triangles = int(rt.asset.last.triangles)
    rt.clusters = int(rt.asset.last.clusters)
    return rt


def update_for_render(scene, depsgraph=None):
    view = camera_view(scene, depsgraph)
    if view is None:
        return
    for obj in proxies(scene):
        if obj.hide_render:
            continue
        # final renders never cull: off-screen geometry still casts shadows and shows in reflections
        mode = "FULL" if obj.vgeo.offscreen == "FULL" else "COARSEN"
        apply_cut(obj, [view], obj.vgeo.render_pixel_error, mode, obj.vgeo.offscreen_scale)


# ---------------------------------------------------------------- live loop

def _tick():
    if _rendering:
        return 0.25
    try:
        objs = [o for o in proxies() if not o.vgeo.freeze]
        if not objs:
            return 0.5
        views = viewport_views()
        if not views:
            return 0.25
        vl = bpy.context.view_layer
        visible = []
        for obj in objs:
            try:
                if vl is None or obj.visible_get(view_layer=vl):
                    visible.append(obj)
            except RuntimeError:
                continue
        busy = False
        budget = TICK_BUDGET / max(1, len(visible))
        for obj in visible:
            v = obj.vgeo
            busy |= stream_step(obj, views, v.pixel_error, v.offscreen, v.offscreen_scale, budget)
        # keep cranking (UI events still run between ticks) until the swap lands
        return 0.0 if busy else TICK
    except Exception as e:  # never let the timer die
        print("VGEO stream:", e)
        return 1.0


@persistent
def _on_render_pre(scene, depsgraph=None):
    global _rendering
    _rendering = True
    try:
        update_for_render(scene, depsgraph)
    except Exception as e:
        print("VGEO render cut failed:", e)


@persistent
def _on_render_done(scene, depsgraph=None):
    global _rendering
    _rendering = False
    invalidate_all()


@persistent
def _on_load(_a=None, _b=None):
    invalidate_all(close=True)


@persistent
def _on_undo(_a=None, _b=None):
    invalidate_all()


def new_uid():
    return uuid.uuid4().hex[:12]


def register():
    h = bpy.app.handlers
    for lst, fn in ((h.render_pre, _on_render_pre), (h.render_complete, _on_render_done),
                    (h.render_cancel, _on_render_done), (h.load_post, _on_load),
                    (h.undo_post, _on_undo), (h.redo_post, _on_undo)):
        if fn not in lst:
            lst.append(fn)
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.5, persistent=True)


def unregister():
    h = bpy.app.handlers
    for lst, fn in ((h.render_pre, _on_render_pre), (h.render_complete, _on_render_done),
                    (h.render_cancel, _on_render_done), (h.load_post, _on_load),
                    (h.undo_post, _on_undo), (h.redo_post, _on_undo)):
        while fn in lst:
            lst.remove(fn)
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    invalidate_all(close=True)
