"""Turn a Blender mesh object into a virtualized (.vgeo) proxy."""

import os
import threading

import bpy
import numpy as np

from . import native, stream


def mesh_arrays(obj, depsgraph):
    """Per-corner triangle arrays of the evaluated mesh (modifiers applied), in object space."""
    ev = obj.evaluated_get(depsgraph)
    me = ev.to_mesh()
    try:
        me.calc_loop_triangles()
        T = len(me.loop_triangles)
        if T == 0:
            raise ValueError(f"'{obj.name}' has no faces")
        tl = np.empty(T * 3, np.int32)
        me.loop_triangles.foreach_get("loops", tl)
        tp = np.empty(T, np.int32)
        me.loop_triangles.foreach_get("polygon_index", tp)
        cv = np.empty(len(me.loops), np.int32)
        me.loops.foreach_get("vertex_index", cv)
        co = np.empty(len(me.vertices) * 3, np.float32)
        me.vertices.foreach_get("co", co)
        cn = np.empty(len(me.loops) * 3, np.float32)
        me.corner_normals.foreach_get("vector", cn)
        positions = co.reshape(-1, 3)[cv[tl]]
        normals = cn.reshape(-1, 3)[tl]
        uvs = None
        layer = me.uv_layers.active
        if layer is not None:
            uv = np.empty(len(me.loops) * 2, np.float32)
            layer.data.foreach_get("uv", uv)
            uvs = uv.reshape(-1, 2)[tl]
        pm = np.empty(len(me.polygons), np.int32)
        me.polygons.foreach_get("material_index", pm)
        slots = max(1, len(obj.material_slots))
        materials = np.clip(pm[tp], 0, slots - 1).astype(np.uint16)
        return positions, normals, uvs, materials
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
            pos, nrm, uvs, mats = self.arrays
            self.result = native.build(self.path, pos, nrm, uvs, mats, self.material_names,
                                       max_triangles=self.max_triangles, progress=self._progress)
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
            stream.apply_cut(proxy, views, v.pixel_error, frustum=v.frustum_cull)
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
