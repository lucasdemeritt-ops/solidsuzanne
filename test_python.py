#!/usr/bin/env python3
"""Test VGEO Python bindings"""

import sys
sys.path.insert(0, r"C:\Users\lucas\onedrive\documents\works\code\vgeo\NewRepo\build\src\python\Release")

import vgeo_native
import numpy as np

print(f"VGEO Native version: {vgeo_native.__version__}")

# Create scene and load a model
scene = vgeo_native.Scene()
transform = np.eye(4, dtype=np.float32).flatten()
obj_id = scene.add_object_from_file(
    r"C:\Users\lucas\onedrive\documents\works\code\vgeo\NewRepo\build\cube.vgeo",
    transform,
    "cube"
)
print(f"Added object {obj_id}")
print(f"Object count: {scene.object_count()}")
print(f"Bounds: {scene.get_object_bounds(obj_id)}")
print(f"Stats: {scene.get_stats()}")

# Query geometry for Cycles export
camera = vgeo_native.Camera()
camera.set_position(0, 0, 10)
camera.set_target(0, 0, 0)
camera.update()

geom = vgeo_native.query_visible_geometry(scene, camera, 1.0)
print(f"Visible triangles: {geom['triangle_count']}, vertices: {geom['vertex_count']}")
print(f"Position array shape: {geom['positions'].shape}")
print(f"Normal array shape: {geom['normals'].shape}")

print("\nPython bindings working correctly!")
