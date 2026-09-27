"""Instancing demo: thousands of copies of a dense virtualized rock, each at its own LOD.

    blender -b --factory-startup --python examples/scatter_demo.py -- --count 2000 --out DIR

Saves scatter_demo.blend (open it in the UI to navigate) and renders EEVEE and Cycles stills.
"""

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
COUNT = int(argv[argv.index("--count") + 1]) if "--count" in argv else 2000
OUT = os.path.abspath(argv[argv.index("--out") + 1] if "--out" in argv else os.path.join(REPO, "build-demo"))
os.makedirs(OUT, exist_ok=True)


def principled(name, color, rough):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
    b.inputs["Base Color"].default_value = (*color, 1)
    b.inputs["Roughness"].default_value = rough
    m.diffuse_color = (*color, 1)
    return m


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    vgeo.register()
    scene = bpy.context.scene
    scene.render.resolution_x, scene.render.resolution_y = 1600, 900

    # a dense rock: 1.3M triangles
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=9, radius=1.0)
    rock = bpy.context.active_object
    rock.name = "Boulder"
    bpy.ops.object.shade_smooth()
    tex = bpy.data.textures.new("rock", "CLOUDS")
    tex.noise_scale = 0.4
    tex.noise_depth = 6
    d = rock.modifiers.new("disp", "DISPLACE")
    d.texture = tex
    d.strength = 0.45
    rock.scale = (1.0, 1.0, 0.6)
    rock.data.materials.append(principled("Granite", (0.42, 0.40, 0.37), 0.75))
    while rock.data.uv_layers:
        rock.data.uv_layers.remove(rock.data.uv_layers[0])

    bpy.ops.mesh.primitive_plane_add(size=300, location=(0, 0, 0))
    ground = bpy.context.active_object
    ground.name = "Meadow"
    ground.data.materials.append(principled("Meadow", (0.18, 0.26, 0.10), 0.95))

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "scatter_demo.blend"))
    for o in scene.objects:
        o.select_set(o is rock)
    bpy.context.view_layer.objects.active = rock
    t0 = time.perf_counter()
    bpy.ops.vgeo.virtualize()
    proxy = bpy.context.active_object
    print(f"[scatter] virtualized in {time.perf_counter() - t0:.1f}s")
    proxy.location = (0, -6, 0.2)

    for o in scene.objects:
        o.select_set(o in (proxy, ground))
    bpy.context.view_layer.objects.active = proxy
    t0 = time.perf_counter()
    bpy.ops.vgeo.scatter(count=COUNT, seed=7, scale_min=0.3, scale_max=2.2)
    inst = bpy.context.active_object
    print(f"[scatter] {COUNT} placements + {len(inst.vgeo_inst.levels.objects)} level meshes in "
          f"{time.perf_counter() - t0:.1f}s")

    cam = bpy.data.objects.new("Camera", bpy.data.cameras.new("Camera"))
    scene.collection.objects.link(cam)
    cam.location = (-18, -70, 6)
    cam.rotation_euler = (Vector((20, 60, 0)) - cam.location).to_track_quat('-Z', 'Y').to_euler()
    cam.data.lens = 32
    scene.camera = cam
    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
    sun.data.energy = 4
    sun.data.angle = 0.02
    sun.rotation_euler = (1.05, 0.15, 2.4)
    scene.collection.objects.link(sun)
    world = bpy.data.worlds.new("Sky")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.45, 0.55, 0.7, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.6
    scene.world = world
    scene.view_settings.view_transform = 'AgX'

    view = stream.camera_view(scene)
    instances.update(inst, [view], 1.0)
    lv = np.empty(COUNT, np.int32)
    inst.data.attributes["vgeo_level"].data.foreach_get("value", lv)
    full = COUNT * inst["vgeo_level_tris"][0]
    print(f"[scatter] 1 px cut: {inst.vgeo_inst.shown_triangles:,} of {full:,} triangles "
          f"({100 * inst.vgeo_inst.shown_triangles / full:.2f}%), levels {lv.min()}..{lv.max()}")
    bpy.ops.wm.save_mainfile()
    for eng, fname, samples in (("BLENDER_EEVEE", "scatter_eevee.png", 0), ("CYCLES", "scatter_cycles.png", 64)):
        scene.render.engine = eng
        if eng == "CYCLES":
            scene.cycles.samples = samples
            prefs = bpy.context.preferences.addons["cycles"].preferences
            try:
                prefs.compute_device_type = "OPTIX"
                prefs.get_devices()
                for dev in prefs.devices:
                    dev.use = dev.type == "OPTIX"
                scene.cycles.device = "GPU"
            except TypeError:
                pass
        scene.render.filepath = os.path.join(OUT, fname)
        t0 = time.perf_counter()
        bpy.ops.render.render(write_still=True)
        print(f"[scatter] {eng}: {time.perf_counter() - t0:.1f}s, {inst.vgeo_inst.shown_triangles:,} triangles")
    vgeo.unregister()


main()
