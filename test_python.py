#!/usr/bin/env python3
"""Test VGEO Python bindings.

Usage: python3 test_python.py [path/to/model.vgeo]

The build directory can be overridden with the VGEO_BUILD_DIR env var.
"""

import os
import sys
import glob

REPO = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.environ.get("VGEO_BUILD_DIR", os.path.join(REPO, "build"))

# Locate the native module across single-config and MSVC build layouts
for candidate in [
    os.path.join(BUILD_DIR, "src", "python"),
    os.path.join(BUILD_DIR, "src", "python", "Release"),
    os.path.join(BUILD_DIR, "src", "python", "Debug"),
]:
    if glob.glob(os.path.join(candidate, "vgeo_native*")):
        sys.path.insert(0, candidate)
        break

import vgeo_native
import numpy as np

print(f"VGEO Native version: {vgeo_native.__version__}")

vgeo_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD_DIR, "cube.vgeo")
if not os.path.exists(vgeo_path):
    print(f"No .vgeo file at {vgeo_path} - convert one with vgeo_build first, "
          f"or pass a path: python3 test_python.py model.vgeo")
    sys.exit(1)

# Create scene and load a model
scene = vgeo_native.Scene()
transform = np.eye(4, dtype=np.float32).flatten()
obj_id = scene.add_object_from_file(vgeo_path, transform, "test_object")
if obj_id < 0:
    print(f"Failed to load {vgeo_path}")
    sys.exit(1)

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
