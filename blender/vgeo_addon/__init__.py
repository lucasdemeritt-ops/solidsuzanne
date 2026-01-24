"""
VGEO Blender Addon
Virtualized Geometry export and preview for Blender
"""

bl_info = {
    "name": "VGEO - Virtualized Geometry",
    "author": "VGEO Team",
    "version": (0, 1, 0),
    "blender": (4, 0, 0),
    "location": "View3D > Sidebar > VGEO",
    "description": "Export and preview virtualized geometry with billions of triangles",
    "category": "Import-Export",
}

import bpy
from bpy.props import BoolProperty, FloatProperty, StringProperty, EnumProperty

# Addon preferences
class VGEOPreferences(bpy.types.AddonPreferences):
    bl_idname = __name__

    viewer_path: StringProperty(
        name="Viewer Path",
        description="Path to vgeo_viewer executable",
        default="",
        subtype='FILE_PATH'
    )

    auto_export: BoolProperty(
        name="Auto Export on Preview",
        description="Automatically export when starting preview",
        default=True
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "viewer_path")
        layout.prop(self, "auto_export")


# Export operator
class VGEO_OT_Export(bpy.types.Operator):
    bl_idname = "vgeo.export"
    bl_label = "Export to VGEO"
    bl_description = "Export selected objects to .vgeo format"

    filepath: StringProperty(subtype='FILE_PATH')

    def execute(self, context):
        # TODO: Implement export
        # 1. Get selected objects
        # 2. Apply modifiers, triangulate
        # 3. Call vgeo_build or built-in converter
        self.report({'INFO'}, f"Export not yet implemented")
        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}


# Preview operator
class VGEO_OT_Preview(bpy.types.Operator):
    bl_idname = "vgeo.preview"
    bl_label = "Preview VGEO"
    bl_description = "Launch VGEO viewer with camera sync"

    _timer = None
    _process = None

    def execute(self, context):
        # TODO: Implement preview
        # 1. Export if needed
        # 2. Launch viewer process
        # 3. Start camera sync timer
        self.report({'INFO'}, "Preview not yet implemented")
        return {'FINISHED'}


# Panel
class VGEO_PT_Panel(bpy.types.Panel):
    bl_label = "VGEO"
    bl_idname = "VGEO_PT_panel"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "VGEO"

    def draw(self, context):
        layout = self.layout

        # Export section
        box = layout.box()
        box.label(text="Export", icon='EXPORT')
        box.operator("vgeo.export", text="Export Selected")

        # Preview section
        box = layout.box()
        box.label(text="Preview", icon='PLAY')
        row = box.row()
        row.operator("vgeo.preview", text="Start Preview")

        # Settings
        box = layout.box()
        box.label(text="Settings", icon='PREFERENCES')
        box.prop(context.scene, "vgeo_error_threshold")


# Scene properties
def register_properties():
    bpy.types.Scene.vgeo_error_threshold = FloatProperty(
        name="Error Threshold",
        description="Screen-space error threshold in pixels",
        default=1.0,
        min=0.1,
        max=10.0
    )


def unregister_properties():
    del bpy.types.Scene.vgeo_error_threshold


# Registration
classes = (
    VGEOPreferences,
    VGEO_OT_Export,
    VGEO_OT_Preview,
    VGEO_PT_Panel,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    register_properties()


def unregister():
    unregister_properties()
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
