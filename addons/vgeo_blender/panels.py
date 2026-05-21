"""
VGEO UI Panels
Render settings and object info panels
"""

import bpy
from bpy.types import Panel

# Try to import native module
try:
    import vgeo_native
    NATIVE_AVAILABLE = True
except ImportError:
    NATIVE_AVAILABLE = False


class VGEO_PT_render_settings(Panel):
    """VGEO Render Settings"""
    bl_label = "VGEO Settings"
    bl_idname = "VGEO_PT_render_settings"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "render"
    COMPAT_ENGINES = {'VGEO'}

    @classmethod
    def poll(cls, context):
        return context.engine == 'VGEO'

    def draw(self, context):
        layout = self.layout

        if not NATIVE_AVAILABLE:
            layout.label(text="Native module not loaded!", icon='ERROR')
            layout.label(text="Check console for details.")
            return

        layout.label(text=f"VGEO v{vgeo_native.__version__}", icon='RENDER_STILL')

        box = layout.box()
        box.label(text="LOD Settings", icon='MOD_DECIM')

        row = box.row()
        row.label(text="Error Threshold:")
        row.label(text="1.0 px")  # TODO: Make this a property

        row = box.row()
        row.label(text="Max LOD Level:")
        row.label(text="Auto")


class VGEO_PT_object_info(Panel):
    """VGEO Object Information"""
    bl_label = "VGEO Info"
    bl_idname = "VGEO_PT_object_info"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return obj and obj.get("vgeo_path") is not None

    def draw(self, context):
        layout = self.layout
        obj = context.active_object

        box = layout.box()
        box.label(text="VGEO Asset", icon='MESH_DATA')

        vgeo_path = obj.get("vgeo_path", "")
        meshlets = obj.get("vgeo_meshlets", 0)
        clusters = obj.get("vgeo_clusters", 0)

        col = box.column(align=True)
        col.label(text=f"Path: {vgeo_path}")
        col.label(text=f"Meshlets: {meshlets:,}")
        col.label(text=f"Clusters: {clusters:,}")

        layout.operator("vgeo.reload", text="Reload Asset", icon='FILE_REFRESH')


class VGEO_PT_scene_stats(Panel):
    """VGEO Scene Statistics"""
    bl_label = "VGEO Scene Stats"
    bl_idname = "VGEO_PT_scene_stats"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'VGEO'

    def draw(self, context):
        layout = self.layout

        # Convert button — always visible regardless of native module
        obj = context.active_object
        col = layout.column(align=True)
        col.label(text="Convert", icon='EXPORT')
        row = col.row()
        row.enabled = obj is not None and obj.type == 'MESH'
        row.operator("vgeo.convert", text="Active Mesh → VGEO", icon='FILE_TICK')

        layout.separator()

        if not NATIVE_AVAILABLE:
            layout.label(text="Native module not loaded!", icon='ERROR')
            return

        # Count VGEO objects in scene
        vgeo_objects = [obj for obj in context.scene.objects if obj.get("vgeo_path")]

        box = layout.box()
        box.label(text="Scene Overview", icon='SCENE_DATA')

        col = box.column(align=True)
        col.label(text=f"VGEO Objects: {len(vgeo_objects)}")

        total_meshlets = sum(obj.get("vgeo_meshlets", 0) for obj in vgeo_objects)
        total_clusters = sum(obj.get("vgeo_clusters", 0) for obj in vgeo_objects)

        col.label(text=f"Total Meshlets: {total_meshlets:,}")
        col.label(text=f"Total Clusters: {total_clusters:,}")

        layout.separator()
        box2 = layout.box()
        box2.label(text="Debug", icon='SHADING_WIRE')
        box2.prop(context.scene, "vgeo_debug_meshlets", text="Meshlet Colors")


# Panel list
classes = [
    VGEO_PT_render_settings,
    VGEO_PT_object_info,
    VGEO_PT_scene_stats,
]


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    print("VGEO Panels: Registered")


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
    print("VGEO Panels: Unregistered")
