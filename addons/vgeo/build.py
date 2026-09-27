"""Turn a Blender mesh object into a virtualized (.vgeo) proxy."""

import os
import threading

import bpy
import numpy as np

from . import native, stream


def mesh_arrays(obj, depsgraph):
    """Triangle arrays of the evaluated mesh (modifiers applied), in object space.

    Returns a dict for native.build. Smooth meshes without UV seams use the
    indexed layout (one row per vertex); anything with hard edges, split
    normals or UV seams falls back to one row per triangle corner.
    """
    ev = obj.evaluated_get(depsgraph)
    me = ev.to_mesh()
    try:
        me.calc_loop_triangles()
        T = len(me.loop_triangles)
        if T == 0:
            raise ValueError(f"'{obj.name}' has no faces")
        V, C = len(me.vertices), len(me.loops)
        tl = np.empty(T * 3, np.int32)
        me.loop_triangles.foreach_get("loops", tl)
        tp = np.empty(T, np.int32)
        me.loop_triangles.foreach_get("polygon_index", tp)
        cv = np.empty(C, np.int32)
        me.loops.foreach_get("vertex_index", cv)
        co = np.empty(V * 3, np.float32)
        me.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3)
        cn = np.empty(C * 3, np.float32)
        me.corner_normals.foreach_get("vector", cn)
        cn = cn.reshape(-1, 3)
        uv = None
        layer = me.uv_layers.active
        if layer is not None:
            uv = np.empty(C * 2, np.float32)
            layer.data.foreach_get("uv", uv)
            uv = uv.reshape(-1, 2)
        pm = np.empty(len(me.polygons), np.int32)
        me.polygons.foreach_get("material_index", pm)
        slots = max(1, len(obj.material_slots))
        materials = np.clip(pm[tp], 0, slots - 1).astype(np.uint16)

        # per-vertex attributes taken from any corner of each vertex
        first = np.full(V, -1, np.int64)
        first[cv[::-1]] = np.arange(C - 1, -1, -1)
        used = first >= 0
        first = np.where(used, first, 0)
        vn = cn[first]
        smooth = np.allclose(cn, vn[cv], atol=1e-5)
        seamless = uv is None or np.allclose(uv, uv[first][cv], atol=1e-6)
        if smooth and seamless:
            return {"positions": co, "normals": vn, "uvs": None if uv is None else uv[first],
                    "materials": materials, "indices": cv[tl].astype(np.uint32)}
        return {"positions": co[cv[tl]], "normals": cn[tl], "uvs": None if uv is None else uv[tl],
                "materials": materials, "indices": None}
    finally:
        ev.to_mesh_clear()


def default_path(obj, uid):
    name = f"{bpy.path.clean_name(obj.name)}_{uid[:6]}.vgeo"
    if bpy.data.filepath:
        folder = bpy.path.abspath("//vgeo")
        os.makedirs(folder, exist_ok=True)
        return os.path.join(folder, name), "//vgeo/" + name
    folder = bpy.utils.user_resource('DATAFILES', path="vgeo", create=True)
    full = os.path.join(folder, name)
    return full, full


class Job:
    """Runs native.build on a worker thread; the GIL is released inside the DLL."""

    def __init__(self, arrays, path, material_names, max_triangles):
        self.arrays = arrays
        self.path = path
        self.material_names = material_names
        self.max_triangles = max_triangles
        self.stage = 0
        self.fraction = 0.0
        self.cancel = False
        self.result = None
        self.error = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _progress(self, stage, fraction):
        self.stage, self.fraction = stage, fraction
        return self.cancel

    def _run(self):
        try:
            a = self.arrays
            self.result = native.build(self.path, a["positions"], a["normals"], a["uvs"], a["materials"],
                                       self.material_names, max_triangles=self.max_triangles,
                                       progress=self._progress, indices=a["indices"])
        except BaseException as e:
            self.error = e
        finally:
            self.arrays = None

    @property
    def overall(self):
        # welding is fast, the DAG dominates
        return {0: 0.05 * self.fraction, 1: 0.05 + 0.9 * self.fraction, 2: 0.95 + 0.05 * self.fraction}.get(self.stage, 0.0)


def create_proxy(context, src, path_setting, uid, build_stats, remove_source=False):
    """Replace src with a streaming proxy that has the same transform, parent, collections and materials."""
    me = bpy.data.meshes.new(f"{src.data.name} VGEO")
    for slot in src.material_slots:
        me.materials.append(slot.material)
    proxy = bpy.data.objects.new(f"{src.name} VGEO", me)
    for col in src.users_collection:
        col.objects.link(proxy)
    proxy.parent = src.parent
    proxy.parent_type = src.parent_type
    proxy.matrix_parent_inverse = src.matrix_parent_inverse.copy()
    proxy.matrix_basis = src.matrix_basis.copy()

    v = proxy.vgeo
    v.uid = uid
    v.path = path_setting
    v.source_triangles = int(build_stats.get("source_triangles", 0))
    v.file_bytes = int(build_stats.get("file_bytes", 0))
    v.collection = bpy.data.collections.new(f".vgeo {uid}")
    stream._attach_modifier(proxy)

    # a first cut so there is something to see before the live loop runs
    rt = stream.runtime_for(proxy)
    if rt.asset is not None:
        views = stream.viewport_views() if context.window_manager.windows else []
        if views:
            stream.apply_cut(proxy, views, v.pixel_error, v.offscreen, v.offscreen_scale)
        else:
            stream.apply_level(proxy, -1)

    if remove_source:
        src_mesh = src.data
        bpy.data.objects.remove(src)
        if src_mesh.users == 0:
            bpy.data.meshes.remove(src_mesh)
    else:
        v.source = src
        src.hide_set(True)
        src.hide_render = True

    for o in context.view_layer.objects:
        o.select_set(False)
    proxy.select_set(True)
    context.view_layer.objects.active = proxy
    # handlers modify scene data during F12 renders; that needs the interface locked
    context.scene.render.use_lock_interface = True
    return proxy
