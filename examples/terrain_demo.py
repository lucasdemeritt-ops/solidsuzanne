"""Large-scale VGEO demo: a procedural mountain range, virtualized and rendered.

    blender -b --factory-startup --python examples/terrain_demo.py -- --res 4096 --out DIR [--raw] [--preview]

--res     grid vertices per side (4096 -> 33.5M triangles)
--raw     also render the raw (non-virtualized) mesh in Cycles for comparison
--preview small, fast EEVEE renders only
"""

import os
import sys
import time

import bpy
from mathutils import Vector

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "addons"))
import vgeo  # noqa: E402
from vgeo import stream  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def arg(name, default):
    return type(default)(argv[argv.index(name) + 1]) if name in argv else default


RES = arg("--res", 2048)
OUT = arg("--out", os.path.join(REPO, "build-demo"))
CHUNKS = arg("--chunks", 0)
RAW = "--raw" in argv
PREVIEW = "--preview" in argv
SIZE = 4000.0
PEAK = 750.0
os.makedirs(OUT, exist_ok=True)

_stats = {}


def log(msg):
    print(f"[demo {time.strftime('%H:%M:%S')}] {msg}", flush=True)


def terrain_nodes():
    ng = bpy.data.node_groups.new("Mountains", "GeometryNodeTree")
    ng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    n, L = ng.nodes, ng.links
    grid = n.new("GeometryNodeMeshGrid")
    grid.inputs["Size X"].default_value = SIZE
    grid.inputs["Size Y"].default_value = SIZE
    grid.inputs["Vertices X"].default_value = RES
    grid.inputs["Vertices Y"].default_value = RES
    pos = n.new("GeometryNodeInputPosition")
    scale = n.new("ShaderNodeVectorMath")
    scale.operation = 'SCALE'
    scale.inputs["Scale"].default_value = 1.0 / 1400.0
    L.new(pos.outputs[0], scale.inputs[0])

    ridge = n.new("ShaderNodeTexNoise")
    ridge.noise_type = 'RIDGED_MULTIFRACTAL'
    ridge.noise_dimensions = '3D'
    ridge.inputs["Scale"].default_value = 1.0
    ridge.inputs["Detail"].default_value = 14.0
    ridge.inputs["Roughness"].default_value = 0.52
    ridge.inputs["Lacunarity"].default_value = 2.05
    ridge.inputs["Offset"].default_value = 0.92
    ridge.inputs["Gain"].default_value = 2.4
    L.new(scale.outputs[0], ridge.inputs["Vector"])

    # broad shape: a valley running through the middle, peaks toward the back
    broad = n.new("ShaderNodeTexNoise")
    broad.inputs["Scale"].default_value = 0.35
    broad.inputs["Detail"].default_value = 2.0
    L.new(scale.outputs[0], broad.inputs["Vector"])
    sep = n.new("ShaderNodeSeparateXYZ")
    L.new(pos.outputs[0], sep.inputs[0])
    back = n.new("ShaderNodeMapRange")
    back.inputs["From Min"].default_value = -SIZE * 0.5
    back.inputs["From Max"].default_value = SIZE * 0.5
    back.inputs["To Min"].default_value = 0.15
    back.inputs["To Max"].default_value = 1.0
    L.new(sep.outputs["Y"], back.inputs["Value"])

    # ridges sharpened by a power, scaled up toward the back, plus rolling foothills
    sharp = n.new("ShaderNodeMath")
    sharp.operation = 'POWER'
    sharp.inputs[1].default_value = 1.6
    L.new(ridge.outputs["Fac"], sharp.inputs[0])
    m1 = n.new("ShaderNodeMath")
    m1.operation = 'MULTIPLY'
    L.new(sharp.outputs[0], m1.inputs[0])
    L.new(back.outputs["Result"], m1.inputs[1])
    ridge_h = n.new("ShaderNodeMath")
    ridge_h.operation = 'MULTIPLY'
    ridge_h.inputs[1].default_value = 380.0
    L.new(m1.outputs[0], ridge_h.inputs[0])
    hills = n.new("ShaderNodeMath")
    hills.operation = 'MULTIPLY'
    hills.inputs[1].default_value = 160.0
    L.new(broad.outputs["Fac"], hills.inputs[0])
    height = n.new("ShaderNodeMath")
    height.operation = 'ADD'
    L.new(ridge_h.outputs[0], height.inputs[0])
    L.new(hills.outputs[0], height.inputs[1])
    # (h - floor) * scale, set after a first evaluation so heights run 0..PEAK metres
    floor = n.new("ShaderNodeMath")
    floor.name = "floor"
    floor.operation = 'SUBTRACT'
    floor.inputs[1].default_value = 0.0
    L.new(height.outputs[0], floor.inputs[0])
    norm = n.new("ShaderNodeMath")
    norm.name = "normalize"
    norm.operation = 'MULTIPLY'
    norm.inputs[1].default_value = 1.0
    L.new(floor.outputs[0], norm.inputs[0])
    off = n.new("ShaderNodeCombineXYZ")
    L.new(norm.outputs[0], off.inputs["Z"])

    setp = n.new("GeometryNodeSetPosition")
    L.new(grid.outputs["Mesh"], setp.inputs["Geometry"])
    L.new(off.outputs[0], setp.inputs["Offset"])
    store = n.new("GeometryNodeStoreNamedAttribute")
    store.data_type = 'FLOAT2'
    store.domain = 'CORNER'
    store.inputs["Name"].default_value = "UVMap"
    L.new(setp.outputs[0], store.inputs["Geometry"])
    L.new(grid.outputs["UV Map"], store.inputs["Value"])
    smooth = n.new("GeometryNodeSetShadeSmooth")
    L.new(store.outputs[0], smooth.inputs["Geometry"])
    out = n.new("NodeGroupOutput")
    L.new(smooth.outputs[0], out.inputs[0])
    return ng


def terrain_material():
    mat = bpy.data.materials.new("Mountain")
    mat.use_nodes = True
    nt = mat.node_tree
    n, L = nt.nodes, nt.links
    bsdf = n["Principled BSDF"]
    geo = n.new("ShaderNodeNewGeometry")
    sepn = n.new("ShaderNodeSeparateXYZ")
    L.new(geo.outputs["Normal"], sepn.inputs[0])
    sepp = n.new("ShaderNodeSeparateXYZ")
    L.new(geo.outputs["Position"], sepp.inputs[0])

    # breakup noise
    tc = n.new("ShaderNodeTexCoord")
    brk = n.new("ShaderNodeTexNoise")
    brk.inputs["Scale"].default_value = 0.02
    brk.inputs["Detail"].default_value = 8
    L.new(tc.outputs["Object"], brk.inputs["Vector"])

    def ramp(src, stops, name):
        r = n.new("ShaderNodeValToRGB")
        r.label = name
        els = r.color_ramp.elements
        els[0].position, els[0].color = stops[0]
        els[1].position, els[1].color = stops[-1]
        for p, c in stops[1:-1]:
            e = els.new(p)
            e.color = c
        L.new(src, r.inputs[0])
        return r

    # slope 0 = flat, 1 = vertical ; perturb with noise so bands are not clean lines
    slope = n.new("ShaderNodeMath")
    slope.operation = 'SUBTRACT'
    slope.inputs[0].default_value = 1.0
    L.new(sepn.outputs["Z"], slope.inputs[1])
    slope_n = n.new("ShaderNodeMath")
    slope_n.operation = 'MULTIPLY_ADD'
    L.new(brk.outputs["Fac"], slope_n.inputs[0])
    slope_n.inputs[1].default_value = 0.18
    L.new(slope.outputs[0], slope_n.inputs[2])

    ground = ramp(brk.outputs["Fac"], [(0.3, (0.035, 0.06, 0.02, 1)), (0.5, (0.09, 0.10, 0.035, 1)),
                                       (0.72, (0.17, 0.14, 0.08, 1))], "grass")
    rock = ramp(brk.outputs["Fac"], [(0.3, (0.16, 0.15, 0.14, 1)), (0.7, (0.36, 0.33, 0.30, 1))], "rock")
    rockmix = ramp(slope_n.outputs[0], [(0.12, (0, 0, 0, 1)), (0.2, (1, 1, 1, 1))], "slope mask")
    mix1 = n.new("ShaderNodeMix")
    mix1.data_type = 'RGBA'
    L.new(rockmix.outputs["Color"], mix1.inputs["Factor"])
    L.new(ground.outputs["Color"], mix1.inputs["A"])
    L.new(rock.outputs["Color"], mix1.inputs["B"])

    # snow: high and not too steep
    hn = n.new("ShaderNodeMath")
    hn.operation = 'MULTIPLY_ADD'
    L.new(brk.outputs["Fac"], hn.inputs[0])
    hn.inputs[1].default_value = 120.0
    L.new(sepp.outputs["Z"], hn.inputs[2])
    snow_h = n.new("ShaderNodeMapRange")
    snow_h.inputs["From Min"].default_value = PEAK * 0.62
    snow_h.inputs["From Max"].default_value = PEAK * 0.70
    L.new(hn.outputs[0], snow_h.inputs["Value"])
    snow_s = n.new("ShaderNodeMapRange")
    snow_s.inputs["From Min"].default_value = 0.42
    snow_s.inputs["From Max"].default_value = 0.30
    L.new(slope_n.outputs[0], snow_s.inputs["Value"])
    snow = n.new("ShaderNodeMath")
    snow.operation = 'MULTIPLY'
    L.new(snow_h.outputs["Result"], snow.inputs[0])
    L.new(snow_s.outputs["Result"], snow.inputs[1])
    mix2 = n.new("ShaderNodeMix")
    mix2.data_type = 'RGBA'
    L.new(snow.outputs[0], mix2.inputs["Factor"])
    L.new(mix1.outputs["Result"], mix2.inputs["A"])
    mix2.inputs["B"].default_value = (0.85, 0.87, 0.9, 1)
    L.new(mix2.outputs["Result"], bsdf.inputs["Base Color"])

    rough = n.new("ShaderNodeMapRange")
    rough.inputs["To Min"].default_value = 0.85
    rough.inputs["To Max"].default_value = 0.45
    L.new(snow.outputs[0], rough.inputs["Value"])
    L.new(rough.outputs["Result"], bsdf.inputs["Roughness"])

    # fine bump for detail below the mesh resolution
    fine = n.new("ShaderNodeTexNoise")
    fine.inputs["Scale"].default_value = 1.2
    fine.inputs["Detail"].default_value = 6
    L.new(tc.outputs["Object"], fine.inputs["Vector"])
    bump = n.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.35
    bump.inputs["Distance"].default_value = 0.3
    L.new(fine.outputs["Fac"], bump.inputs["Height"])
    L.new(bump.outputs["Normal"], bsdf.inputs["Normal"])

    # aerial perspective: blend toward haze with distance (cheap in both engines)
    camd = n.new("ShaderNodeCameraData")
    k = n.new("ShaderNodeMath")
    k.operation = 'MULTIPLY'
    k.inputs[1].default_value = -1.0 / 3800.0
    L.new(camd.outputs["View Distance"], k.inputs[0])
    ex = n.new("ShaderNodeMath")
    ex.operation = 'EXPONENT'
    L.new(k.outputs[0], ex.inputs[0])
    fog = n.new("ShaderNodeMath")
    fog.operation = 'MULTIPLY_ADD'
    fog.inputs[1].default_value = -0.6
    fog.inputs[2].default_value = 0.6
    L.new(ex.outputs[0], fog.inputs[0])
    haze = n.new("ShaderNodeEmission")
    haze.inputs["Color"].default_value = (0.52, 0.62, 0.78, 1)
    haze.inputs["Strength"].default_value = 0.9
    mix = n.new("ShaderNodeMixShader")
    L.new(fog.outputs[0], mix.inputs["Fac"])
    L.new(bsdf.outputs[0], mix.inputs[1])
    L.new(haze.outputs[0], mix.inputs[2])
    L.new(mix.outputs[0], n["Material Output"].inputs["Surface"])
    return mat


def setup_world(scene):
    world = bpy.data.worlds.new("Sky")
    world.use_nodes = True
    nt = world.node_tree
    sky = nt.nodes.new("ShaderNodeTexSky")
    types = [e.identifier for e in sky.bl_rna.properties["sky_type"].enum_items]
    for t in ("MULTIPLE_SCATTERING", "NISHITA", "SINGLE_SCATTERING"):
        if t in types:
            sky.sky_type = t
            break
    el, rot = 0.22, -1.0   # low sun from the front-left: side light shows relief
    sky.sun_elevation = el
    sky.sun_rotation = rot
    if hasattr(sky, "sun_disc"):
        sky.sun_disc = False  # the Sun lamp provides direct light
    if hasattr(sky, "altitude"):
        sky.altitude = 400.0
    bg = nt.nodes["Background"]
    bg.inputs["Strength"].default_value = 0.35
    nt.links.new(sky.outputs[0], bg.inputs[0])
    scene.world = world

    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
    sun.data.energy = 4.0
    sun.data.angle = 0.01
    sun.data.color = (1.0, 0.86, 0.7)
    # match the sky's sun direction
    from math import cos, sin
    d = Vector((sin(rot) * cos(el), cos(rot) * cos(el), sin(el)))
    sun.rotation_euler = (-d).to_track_quat('-Z', 'Y').to_euler()
    scene.collection.objects.link(sun)


def ground_height(scene, obj, x, y):
    dg = bpy.context.evaluated_depsgraph_get()
    ok, loc, *_ = scene.ray_cast(dg, Vector((x, y, 3000)), Vector((0, 0, -1)))
    return loc.z if ok else 0.0


def place_camera(scene, obj):
    cam = bpy.data.objects.new("Camera", bpy.data.cameras.new("Camera"))
    scene.collection.objects.link(cam)
    cx, cy = -300.0, -1650.0
    cam.location = (cx, cy, ground_height(scene, obj, cx, cy) + 3.0)
    target = Vector((250.0, 900.0, 170.0))
    cam.rotation_euler = (target - cam.location).to_track_quat('-Z', 'Y').to_euler()
    cam.data.lens = 28
    cam.data.clip_start = 0.1
    cam.data.clip_end = 10000
    scene.camera = cam
    return cam


def use_gpu(scene):
    prefs = bpy.context.preferences.addons["cycles"].preferences
    for kind in ("OPTIX", "CUDA"):
        try:
            prefs.compute_device_type = kind
            prefs.get_devices()
            devs = [d for d in prefs.devices if d.type == kind]
            if devs:
                for d in prefs.devices:
                    d.use = d.type == kind
                scene.cycles.device = 'GPU'
                return kind
        except TypeError:
            continue
    return "CPU"


def render(scene, engine, name, samples=64):
    scene.render.engine = engine
    if engine == "CYCLES":
        scene.cycles.samples = samples
        scene.cycles.use_denoising = True
    scene.render.filepath = os.path.join(OUT, name)
    _stats.clear()
    t0 = time.perf_counter()
    bpy.ops.render.render(write_still=True)
    dt = time.perf_counter() - t0
    log(f"render {name}: {dt:.1f}s  {_stats.get('last', '')}")
    return dt


def _on_stats(stats):
    _stats["last"] = stats.split("|")[-3:] if "|" in stats else stats


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    vgeo.register()
    bpy.app.handlers.render_stats.append(_on_stats)
    scene = bpy.context.scene
    scene.render.resolution_x, scene.render.resolution_y = (960, 540) if PREVIEW else (1920, 1080)
    scene.view_settings.view_transform = 'AgX'
    scene.view_settings.look = 'AgX - Punchy' if 'AgX - Punchy' in [
        i.identifier for i in scene.view_settings.bl_rna.properties['look'].enum_items_static] else 'None'
    gpu = use_gpu(scene)
    log(f"Blender {bpy.app.version_string}, Cycles device {gpu}, grid {RES}^2 = {2 * (RES - 1) ** 2:,} triangles")

    me = bpy.data.meshes.new("Mountains")
    obj = bpy.data.objects.new("Mountains", me)
    scene.collection.objects.link(obj)
    mod = obj.modifiers.new("Mountains", 'NODES')
    mod.node_group = terrain_nodes()
    me.materials.append(terrain_material())
    setup_world(scene)
    t0 = time.perf_counter()
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    zs = [v[2] for v in obj.evaluated_get(dg).bound_box]
    mod.node_group.nodes["floor"].inputs[1].default_value = min(zs)
    mod.node_group.nodes["normalize"].inputs[1].default_value = PEAK / max(1e-3, max(zs) - min(zs))
    bpy.context.view_layer.update()
    place_camera(scene, obj)
    log(f"terrain evaluated in {time.perf_counter() - t0:.1f}s")
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, f"terrain_{RES}" + (f"_c{CHUNKS}" if CHUNKS else "") + ".blend"))

    if RAW:
        render(scene, "CYCLES", f"raw_{RES}.png")

    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    t0 = time.perf_counter()
    bpy.ops.vgeo.virtualize(target_chunks=CHUNKS)
    proxy = bpy.context.active_object
    rt = stream.runtime_for(proxy)
    info = rt.asset.info
    log(f"virtualized in {time.perf_counter() - t0:.1f}s: {info['source_triangles']:,} tris, "
        f"{info['cluster_count']:,} clusters, {info['lod_levels']} levels, {rt.asset.chunk_count} chunks, "
        f"file {proxy.vgeo.file_bytes / 2**20:.0f} MB")
    # the source is no longer needed in memory for rendering
    obj.modifiers["Mountains"].show_render = False
    obj.modifiers["Mountains"].show_viewport = False

    view = stream.camera_view(scene)
    for px, mode in ((0.5, "FULL"), (0.5, "COARSEN"), (1.0, "COARSEN"), (1.0, "CULL"), (2.0, "COARSEN")):
        t0 = time.perf_counter()
        stream.apply_cut(proxy, [view], px, mode, 8.0, force=True)
        log(f"cut @ {px}px {mode:8s}: {rt.triangles:,} tris ({100.0 * rt.triangles / info['source_triangles']:.2f}%), "
            f"{rt.clusters:,} clusters, {rt.last_rebuilt} chunks in {(time.perf_counter() - t0) * 1000:.0f} ms")

    proxy.vgeo.lod_colors = True
    scene.display.shading.color_type = 'VERTEX'
    scene.display.shading.light = 'STUDIO'
    render(scene, "BLENDER_WORKBENCH", f"lod_{RES}.png")
    proxy.vgeo.lod_colors = False
    render(scene, "BLENDER_EEVEE", f"eevee_{RES}.png")
    if not PREVIEW:
        render(scene, "CYCLES", f"cycles_{RES}.png", samples=128)
    bpy.ops.wm.save_mainfile()
    vgeo.unregister()


main()
