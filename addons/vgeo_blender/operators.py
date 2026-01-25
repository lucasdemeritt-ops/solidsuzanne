"""
VGEO Operators
Import/export operators for .vgeo files
"""

import bpy
from bpy.types import Operator
from bpy.props import StringProperty, BoolProperty
from bpy_extras.io_utils import ImportHelper
import os

# Try to import native module
try:
    import vgeo_native
    NATIVE_AVAILABLE = True
except ImportError:
    NATIVE_AVAILABLE = False


class VGEO_OT_import(Operator, ImportHelper):
    """Import a VGEO file as a mesh object"""
    bl_idname = "vgeo.import"
    bl_label = "Import VGEO"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".vgeo"
    filter_glob: StringProperty(
        default="*.vgeo",
        options={'HIDDEN'},
    )

    create_proxy: BoolProperty(
        name="Create Proxy Mesh",
        description="Create a simplified proxy mesh for viewport display (when not using VGEO engine)",
        default=True,
    )

    def execute(self, context):
        if not NATIVE_AVAILABLE:
            self.report({'ERROR'}, "VGEO native module not available")
            return {'CANCELLED'}

        # Load the asset to get bounds
        scene = vgeo_native.Scene()
        obj_id = scene.add_object_from_file(self.filepath, None, "temp")

        if obj_id < 0:
            self.report({'ERROR'}, f"Failed to load {self.filepath}")
            return {'CANCELLED'}

        bounds = scene.get_object_bounds(obj_id)
        stats = scene.get_stats()

        # Create an empty or cube as proxy
        if self.create_proxy:
            # Create a simple cube at the bounds
            bpy.ops.mesh.primitive_cube_add()
            obj = context.active_object

            # Scale to match bounds
            min_b, max_b = bounds
            size = [max_b[i] - min_b[i] for i in range(3)]
            center = [(max_b[i] + min_b[i]) / 2 for i in range(3)]

            obj.scale = [s / 2 for s in size]  # Cube is 2x2x2
            obj.location = center
        else:
            # Create an empty
            bpy.ops.object.empty_add(type='CUBE')
            obj = context.active_object

            min_b, max_b = bounds
            size = max(max_b[i] - min_b[i] for i in range(3))
            obj.empty_display_size = size / 2

        # Store VGEO path in custom property
        obj.name = os.path.basename(self.filepath).replace('.vgeo', '')
        obj["vgeo_path"] = self.filepath
        obj["vgeo_meshlets"] = stats.get('total_meshlets', 0)
        obj["vgeo_clusters"] = stats.get('total_clusters', 0)

        self.report({'INFO'}, f"Imported VGEO: {obj.name} ({stats.get('total_meshlets', 0)} meshlets)")
        return {'FINISHED'}


class VGEO_OT_reload(Operator):
    """Reload VGEO file for selected object"""
    bl_idname = "vgeo.reload"
    bl_label = "Reload VGEO"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return obj and obj.get("vgeo_path") is not None

    def execute(self, context):
        obj = context.active_object
        vgeo_path = obj.get("vgeo_path")

        if not vgeo_path or not os.path.exists(vgeo_path):
            self.report({'ERROR'}, "VGEO file not found")
            return {'CANCELLED'}

        # The engine will reload on next view_update
        self.report({'INFO'}, f"Marked for reload: {vgeo_path}")
        return {'FINISHED'}


# Operator list
classes = [
    VGEO_OT_import,
    VGEO_OT_reload,
]


def menu_func_import(self, context):
    self.layout.operator(VGEO_OT_import.bl_idname, text="VGEO (.vgeo)")


def register():
    for cls in classes:
        bpy.utils.register_class(cls)

    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)
    print("VGEO Operators: Registered")


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)

    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)

    print("VGEO Operators: Unregistered")
