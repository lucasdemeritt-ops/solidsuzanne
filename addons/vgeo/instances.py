"""Many copies of one virtualized asset: full scenes from dense assets.

An instancer is a mesh object whose vertices are placements (with optional
per-point rotation and scale attributes). The asset's uniform LOD levels are
built once into shared "level meshes" (each is a complete, crack-free cut).
Every update picks, per placement, the coarsest level whose geometric error
stays under the pixel threshold for the current views, and writes it to an
integer attribute; a Geometry Nodes tree instances the chosen level mesh at
each point. Navigating never rebuilds geometry, and EEVEE and Cycles get
real instances (one copy of each level in memory, however many placements).
"""

import math

import bpy
import numpy as np

from . import native, stream

NODE_GROUP = "VGEO Instances"
LEVEL_ATTR = "vgeo_level"
ROT_ATTR = "vgeo_rot"
SCALE_ATTR = "vgeo_scale"

_levels_cache = {}   # instancer uid -> (errors ndarray, tris ndarray)


def instancers(scene=None):
    objs = scene.objects if scene is not None else bpy.data.objects
    return [o for o in objs if o.type == 'MESH' and o.vgeo_inst.uid and o.vgeo_inst.levels is not None]


def ensure_node_group():
    ng = bpy.data.node_groups.get(NODE_GROUP)
    if ng and ng.bl_idname == "GeometryNodeTree" and "Levels" in ng.interface.items_tree:
        return ng
    ng = bpy.data.node_groups.new(NODE_GROUP, "GeometryNodeTree")
    ng.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    ng.interface.new_socket("Levels", in_out="INPUT", socket_type="NodeSocketCollection")
    ng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    n, L = ng.nodes, ng.links
    gin = n.new("NodeGroupInput")
    info = n.new("GeometryNodeCollectionInfo")
    info.transform_space = "ORIGINAL"
    info.inputs["Separate Children"].default_value = True
    info.inputs["Reset Children"].default_value = True
    L.new(gin.outputs["Levels"], info.inputs["Collection"])

    def named(name, kind):
        a = n.new("GeometryNodeInputNamedAttribute")
        a.data_type = kind
        a.inputs["Name"].default_value = name
        return a

    lvl = named(LEVEL_ATTR, 'INT')
    rot = named(ROT_ATTR, 'FLOAT_VECTOR')
    scl = named(SCALE_ATTR, 'FLOAT')
    e2r = n.new("FunctionNodeEulerToRotation")
    L.new(rot.outputs["Attribute"], e2r.inputs[0])
    iop = n.new("GeometryNodeInstanceOnPoints")
    L.new(gin.outputs["Geometry"], iop.inputs["Points"])
    L.new(info.outputs["Instances"], iop.inputs["Instance"])
    iop.inputs["Pick Instance"].default_value = True
    L.new(lvl.outputs["Attribute"], iop.inputs["Instance Index"])
    L.new(e2r.outputs[0], iop.inputs["Rotation"])
    L.new(scl.outputs["Attribute"], iop.inputs["Scale"])
    out = n.new("NodeGroupOutput")
    L.new(iop.outputs["Instances"], out.inputs["Geometry"])
    return ng


def _socket_id(ng, name):
    return next(i.identifier for i in ng.interface.items_tree
                if getattr(i, "in_out", None) == "INPUT" and i.name == name)


def build_levels(inst, source):
    """Build (or rebuild) the shared level meshes for an instancer from a VGEO proxy."""
    path = stream.asset_path(source)
    asset = native.Asset(path)   # own handle: never disturbs the proxy's live selection
    try:
        errors = asset.level_errors()
        uid = inst.vgeo_inst.uid
        col = inst.vgeo_inst.levels
        if col is None:
            col = bpy.data.collections.new(f".vgeo levels {uid}")
            inst.vgeo_inst.levels = col
        for ob in list(col.objects):   # previous build
            me = ob.data
            bpy.data.objects.remove(ob)
            if me is not None and me.users == 0:
                bpy.data.meshes.remove(me)
        materials = tuple(source.data.materials)
        tris = []
        for depth in range(len(errors)):
            name = f"vgeo.{uid}.L{depth:02d}"
            me = bpy.data.meshes.new(name)
            stream._sync_materials(me, materials)
            data = asset.level_mesh_data(depth)
            stream.fill_mesh(me, data, materials, False)
            ob = bpy.data.objects.new(name, me)
            col.objects.link(ob)
            tris.append(data["tri_count"] if data else 0)
        lo, hi = np.array(asset.info["aabb_min"]), np.array(asset.info["aabb_max"])
        center = (lo + hi) / 2
        inst["vgeo_level_errors"] = [float(e) for e in errors]
        inst["vgeo_level_tris"] = [int(t) for t in tris]
        # bounding sphere around the asset origin (placements put the origin on the point)
        inst["vgeo_radius"] = float(np.linalg.norm(center) + np.linalg.norm(hi - lo) / 2)
        _levels_cache.pop(uid, None)
    finally:
        asset.close()
    _attach(inst)


def _attach(inst):
    ng = ensure_node_group()
    mod = next((m for m in inst.modifiers if m.type == 'NODES' and m.node_group == ng), None)
    if mod is None:
        mod = inst.modifiers.new("VGEO Instances", 'NODES')
        mod.node_group = ng
    mod[_socket_id(ng, "Levels")] = inst.vgeo_inst.levels


def _level_tables(inst):
    uid = inst.vgeo_inst.uid
    t = _levels_cache.get(uid)
    if t is None:
        t = (np.array(inst.get("vgeo_level_errors", [0.0]), dtype=np.float64),
             np.array(inst.get("vgeo_level_tris", [0]), dtype=np.int64))
        _levels_cache[uid] = t
    return t


def _points(inst):
    me = inst.data
    n = len(me.vertices)
    co = np.empty(n * 3, np.float32)
    me.vertices.foreach_get("co", co)
    scale = np.ones(n, np.float32)
    a = me.attributes.get(SCALE_ATTR)
    if a is not None and a.domain == 'POINT' and a.data_type == 'FLOAT':
        a.data.foreach_get("value", scale)
    return co.reshape(-1, 3), scale


def choose_levels(inst, views, pixel_error, mode="COARSEN", offscreen_scale=8.0):
    """Per placement, the coarsest level that keeps the error under the threshold in every view."""
    errors, _tris = _level_tables(inst)
    co, scale = _points(inst)
    n = len(co)
    if n == 0:
        return np.zeros(0, np.int32)
    mw = np.array(inst.matrix_world, dtype=np.float64)
    world = co @ mw[:3, :3].T + mw[:3, 3]
    s = scale.astype(np.float64) * max(abs(x) for x in inst.matrix_world.to_scale())
    radius = float(inst.get("vgeo_radius", 1.0)) * s
    budget = np.full(n, np.inf)
    for view, window, height, persp, clip_start in views:
        cam = np.array(view.inverted().translation, dtype=np.float64)
        t = pixel_error / max(1, height)
        if persp:
            d = np.maximum(np.linalg.norm(world - cam, axis=1) - radius, max(clip_start, 1e-6))
            b = t * d / (window[1][1] * 0.5)
        else:
            b = np.full(n, t * (2.0 / window[1][1] if window[1][1] else 1.0))
        if mode != "FULL":
            planes = np.array(stream._frustum_planes(window @ view), dtype=np.float64)
            inside = np.all(world @ planes[:, :3].T + planes[:, 3] >= -radius[:, None], axis=1)
            if mode == "COARSEN":
                b = np.where(inside, b, b * offscreen_scale)
            else:   # CULL has no meaning for instances that cast shadows; treat as coarsen
                b = np.where(inside, b, b * max(offscreen_scale, 64.0))
        budget = np.minimum(budget, b / s)   # error allowed in asset units
    levels = np.searchsorted(errors, budget, side="right") - 1
    levels = np.clip(levels, int(inst.vgeo_inst.min_level), len(errors) - 1)
    return levels.astype(np.int32)


def apply_levels(inst, levels):
    """Write the level attribute if it changed. Returns True when geometry nodes must re-evaluate."""
    me = inst.data
    a = me.attributes.get(LEVEL_ATTR)
    if a is None or a.domain != 'POINT' or a.data_type != 'INT':
        if a is not None:
            me.attributes.remove(a)
        a = me.attributes.new(LEVEL_ATTR, 'INT', 'POINT')
    cur = np.empty(len(me.vertices), np.int32)
    a.data.foreach_get("value", cur)
    if np.array_equal(cur, levels):
        return False
    a.data.foreach_set("value", levels)
    me.update()
    return True


def triangles_for(inst, levels):
    _errors, tris = _level_tables(inst)
    return int(tris[levels].sum()) if len(levels) else 0


def update(inst, views, pixel_error, mode="COARSEN", offscreen_scale=8.0):
    levels = choose_levels(inst, views, pixel_error, mode, offscreen_scale)
    changed = apply_levels(inst, levels)
    inst.vgeo_inst.shown_triangles = triangles_for(inst, levels)
    return changed


# ---------------------------------------------------------------- scattering

def _euler_from_matrices(m):
    """XYZ Euler angles from rotation matrices (n, 3, 3), matching Blender's convention."""
    sy = np.sqrt(m[:, 0, 0] ** 2 + m[:, 1, 0] ** 2)
    x = np.arctan2(m[:, 2, 1], m[:, 2, 2])
    y = np.arctan2(-m[:, 2, 0], sy)
    z = np.arctan2(m[:, 1, 0], m[:, 0, 0])
    return np.stack([x, y, z], 1)


def scatter_points(surface, count, seed=0, scale_range=(0.7, 1.3), align=True, depsgraph=None):
    """Area-weighted random placements on a mesh surface (world space), with rotation and scale."""
    dg = depsgraph or bpy.context.evaluated_depsgraph_get()
    ev = surface.evaluated_get(dg)
    me = ev.to_mesh()
    try:
        me.calc_loop_triangles()
        T = len(me.loop_triangles)
        if T == 0:
            raise ValueError(f"'{surface.name}' has no faces to scatter on")
        tv = np.empty(T * 3, np.int32)
        me.loop_triangles.foreach_get("vertices", tv)
        co = np.empty(len(me.vertices) * 3, np.float32)
        me.vertices.foreach_get("co", co)
    finally:
        ev.to_mesh_clear()
    mw = np.array(surface.matrix_world, dtype=np.float64)
    co = co.reshape(-1, 3) @ mw[:3, :3].T + mw[:3, 3]
    tri = co[tv.reshape(-1, 3)]
    e1, e2 = tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]
    cr = np.cross(e1, e2)
    area = np.linalg.norm(cr, axis=1)
    rng = np.random.default_rng(seed)
    pick = rng.choice(T, size=count, p=area / area.sum())
    u, v = rng.random(count), rng.random(count)
    flip = u + v > 1
    u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
    pos = tri[pick, 0] + e1[pick] * u[:, None] + e2[pick] * v[:, None]
    nrm = cr[pick] / np.maximum(area[pick], 1e-12)[:, None]
    spin = rng.random(count) * 2 * math.pi
    if align:
        z = nrm
    else:
        z = np.tile([0.0, 0.0, 1.0], (count, 1))
    ref = np.where(np.abs(z[:, 2:3]) < 0.9, [[0.0, 0.0, 1.0]], [[1.0, 0.0, 0.0]])
    x = np.cross(ref, z)
    x /= np.linalg.norm(x, axis=1)[:, None]
    y = np.cross(z, x)
    c, s = np.cos(spin)[:, None], np.sin(spin)[:, None]
    x, y = x * c + y * s, y * c - x * s
    rot = np.stack([x, y, z], 2)   # columns = local axes in world space
    scale = rng.uniform(scale_range[0], scale_range[1], count)
    return pos.astype(np.float32), _euler_from_matrices(rot).astype(np.float32), scale.astype(np.float32)


def create_instancer(context, source, surface, count, seed=0, scale_range=(0.7, 1.3), align=True):
    pos, rot, scale = scatter_points(surface, count, seed, scale_range, align)
    me = bpy.data.meshes.new(f"{source.name} Instances")
    me.vertices.add(len(pos))
    me.vertices.foreach_set("co", pos.ravel())
    me.attributes.new(ROT_ATTR, 'FLOAT_VECTOR', 'POINT').data.foreach_set("vector", rot.ravel())
    me.attributes.new(SCALE_ATTR, 'FLOAT', 'POINT').data.foreach_set("value", scale)
    me.attributes.new(LEVEL_ATTR, 'INT', 'POINT')
    me.update()
    inst = bpy.data.objects.new(f"{source.name} Instances", me)
    for col in surface.users_collection:
        col.objects.link(inst)
    inst.vgeo_inst.uid = stream.new_uid()
    inst.vgeo_inst.source = source
    inst.vgeo_inst.pixel_error = source.vgeo.pixel_error
    inst.vgeo_inst.render_pixel_error = source.vgeo.render_pixel_error
    build_levels(inst, source)
    return inst
