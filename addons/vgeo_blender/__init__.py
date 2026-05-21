"""
VGEO Blender Addon
Virtualized Geometry viewport renderer and Cycles integration
"""

bl_info = {
    "name": "VGEO - Virtualized Geometry",
    "author": "VGEO Project",
    "version": (0, 1, 0),
    "blender": (4, 0, 0),
    "location": "Properties > Render > Render Engine",
    "description": "Nanite-style virtualized geometry for Blender viewport",
    "category": "Render",
}

import bpy
import sys
import os

# Add path to vgeo_native module
def setup_module_path():
    """Add the vgeo_native module to Python path"""
    # Look for vgeo_native.pyd in common locations
    addon_dir = os.path.dirname(os.path.realpath(__file__))

    # Possible locations for the native module
    search_paths = [
        os.path.join(addon_dir, "native"),  # Shipped with addon
        os.path.join(addon_dir, "..", "..", "build", "src", "python", "Debug"),  # Dev build (Debug)
        os.path.join(addon_dir, "..", "..", "build", "src", "python", "Release"),  # Dev build (Release)
    ]

    for path in search_paths:
        if os.path.exists(path):
            if path not in sys.path:
                sys.path.insert(0, path)
                print(f"VGEO: Added module path: {path}")
            break

setup_module_path()

# Try to import native module
try:
    import vgeo_native
    VGEO_NATIVE_AVAILABLE = True
    print(f"VGEO: Native module loaded (version {vgeo_native.__version__})")
except ImportError as e:
    VGEO_NATIVE_AVAILABLE = False
    print(f"VGEO: Native module not available: {e}")

# Import submodules
from . import engine
from . import operators
from . import panels

# Classes to register
classes = []

def register():
    """Register the addon"""
    # Register classes
    for cls in classes:
        bpy.utils.register_class(cls)

    # Register submodules
    engine.register()
    operators.register()
    panels.register()

    print("VGEO: Addon registered")

def unregister():
    """Unregister the addon"""
    # Unregister submodules
    panels.unregister()
    operators.unregister()
    engine.unregister()

    # Unregister classes
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)

    print("VGEO: Addon unregistered")

if __name__ == "__main__":
    register()
