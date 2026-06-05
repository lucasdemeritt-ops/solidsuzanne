"""
VGEO Blender Addon
Virtualized Geometry export and preview for Blender

This addon provides:
- Export of mesh objects to .vgeo format via vgeo_build
- Quick preview of meshes in the VGEO viewer
- Scene export to .vscene manifest format
- Integration with Blender's 3D viewport
"""

bl_info = {
    "name": "VGEO - Virtualized Geometry",
    "author": "VGEO Team",
    "version": (0, 1, 0),
    "blender": (4, 0, 0),
    "location": "View3D > Sidebar > VGEO",
    "description": "Export and preview virtualized geometry with billions of triangles",
    "warning": "",
    "doc_url": "https://github.com/vgeo-project/vgeo",
    "tracker_url": "https://github.com/vgeo-project/vgeo/issues",
    "category": "Import-Export",
}

import bpy
import os
import sys
import json
import tempfile
import subprocess
import shutil
import math
from pathlib import Path
from datetime import datetime
from bpy.props import (
    BoolProperty,
    FloatProperty,
    IntProperty,
    StringProperty,
    EnumProperty,
    PointerProperty,
)
from bpy.types import (
    AddonPreferences,
    Operator,
    Panel,
    PropertyGroup,
)


# ==============================================================================
# Helper Functions
# ==============================================================================

def find_executable(name, preferences=None):
    """
    Locate a VGEO executable (vgeo_build or vgeo_viewer).

    Search order:
    1. Path specified in addon preferences
    2. Same directory as the addon
    3. System PATH
    4. Common installation directories

    Args:
        name: Executable name (without extension)
        preferences: Optional addon preferences object

    Returns:
        Full path to executable or None if not found
    """
    exe_ext = ".exe" if sys.platform == "win32" else ""
    exe_name = f"{name}{exe_ext}"

    # Check preferences first
    if preferences:
        if name == "vgeo_build" and preferences.build_path:
            if os.path.isfile(preferences.build_path):
                return preferences.build_path
        elif name == "vgeo_viewer" and preferences.viewer_path:
            if os.path.isfile(preferences.viewer_path):
                return preferences.viewer_path

    # Check addon directory and parent directories
    addon_dir = os.path.dirname(os.path.realpath(__file__))
    search_paths = [
        addon_dir,
        os.path.dirname(addon_dir),
        os.path.join(os.path.dirname(addon_dir), "bin"),
        os.path.join(os.path.dirname(os.path.dirname(addon_dir)), "bin"),
        os.path.join(os.path.dirname(os.path.dirname(addon_dir)), "build"),
        os.path.join(os.path.dirname(os.path.dirname(addon_dir)), "build", "Release"),
        os.path.join(os.path.dirname(os.path.dirname(addon_dir)), "build", "Debug"),
    ]

    for path in search_paths:
        exe_path = os.path.join(path, exe_name)
        if os.path.isfile(exe_path):
            return exe_path
        # Also check tools subdirectory
        tools_path = os.path.join(path, "tools", exe_name)
        if os.path.isfile(tools_path):
            return tools_path

    # Check system PATH
    result = shutil.which(exe_name)
    if result:
        return result

    # Common installation directories
    if sys.platform == "win32":
        common_paths = [
            os.path.expandvars(r"%LOCALAPPDATA%\vgeo\bin"),
            os.path.expandvars(r"%PROGRAMFILES%\vgeo\bin"),
            os.path.expandvars(r"%PROGRAMFILES(X86)%\vgeo\bin"),
        ]
    else:
        common_paths = [
            os.path.expanduser("~/.local/bin"),
            "/usr/local/bin",
            "/opt/vgeo/bin",
        ]

    for path in common_paths:
        exe_path = os.path.join(path, exe_name)
        if os.path.isfile(exe_path):
            return exe_path

    return None


def get_preferences():
    """Get the addon preferences."""
    addon = bpy.context.preferences.addons.get(__name__)
    if addon:
        return addon.preferences
    return None


def run_vgeo_build(input_path, output_path, options=None):
    """
    Run vgeo_build to convert a mesh to .vgeo format.

    Args:
        input_path: Path to input file (OBJ, PLY, etc.)
        output_path: Path for output .vgeo file
        options: Dictionary of build options
            - max_verts: Maximum vertices per meshlet
            - max_tris: Maximum triangles per meshlet
            - compress: Enable LZ4 compression
            - quantize: Enable position quantization
            - quantize_bits: Bits for quantization (16 or 21)

    Returns:
        Tuple of (success: bool, message: str, stats: dict or None)
    """
    prefs = get_preferences()
    exe_path = find_executable("vgeo_build", prefs)

    if not exe_path:
        return (False, "vgeo_build executable not found. Please set the path in addon preferences.", None)

    if not os.path.isfile(input_path):
        return (False, f"Input file not found: {input_path}", None)

    # Build command line arguments
    cmd = [exe_path, input_path, "-o", output_path]

    if options:
        if options.get("max_verts"):
            cmd.extend(["--max-verts", str(options["max_verts"])])
        if options.get("max_tris"):
            cmd.extend(["--max-tris", str(options["max_tris"])])
        if options.get("compress"):
            cmd.append("--compress")
        if options.get("quantize"):
            cmd.append("--quantize")
            if options.get("quantize_bits"):
                cmd.extend(["--quantize-bits", str(options["quantize_bits"])])

    try:
        # Run the build process
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=300,  # 5 minute timeout
        )

        if result.returncode == 0:
            # Parse stats from output if available
            stats = {}
            for line in result.stdout.split('\n'):
                if ':' in line:
                    key, _, value = line.partition(':')
                    stats[key.strip().lower()] = value.strip()

            return (True, "Export successful", stats)
        else:
            error_msg = result.stderr or result.stdout or "Unknown error"
            return (False, f"vgeo_build failed: {error_msg}", None)

    except subprocess.TimeoutExpired:
        return (False, "vgeo_build timed out after 5 minutes", None)
    except FileNotFoundError:
        return (False, f"Could not execute vgeo_build at: {exe_path}", None)
    except Exception as e:
        return (False, f"Error running vgeo_build: {str(e)}", None)


def export_mesh_to_obj(obj, filepath, apply_modifiers=True, triangulate=True):
    """
    Export a single mesh object to OBJ format.

    Args:
        obj: Blender mesh object to export
        filepath: Output OBJ file path
        apply_modifiers: Whether to apply modifiers
        triangulate: Whether to triangulate the mesh

    Returns:
        Tuple of (success: bool, message: str)
    """
    if obj.type != 'MESH':
        return (False, f"Object '{obj.name}' is not a mesh")

    # Store original selection
    original_active = bpy.context.view_layer.objects.active
    original_selection = [o for o in bpy.context.selected_objects]

    try:
        # Deselect all, select only target object
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj

        # Get evaluated mesh if applying modifiers
        if apply_modifiers:
            depsgraph = bpy.context.evaluated_depsgraph_get()
            obj_eval = obj.evaluated_get(depsgraph)
            mesh = obj_eval.to_mesh()
        else:
            mesh = obj.data.copy()

        # Triangulate if needed
        if triangulate:
            import bmesh
            bm = bmesh.new()
            bm.from_mesh(mesh)
            bmesh.ops.triangulate(bm, faces=bm.faces[:])
            bm.to_mesh(mesh)
            bm.free()

        # Create temporary object with the processed mesh
        temp_obj = bpy.data.objects.new("_vgeo_export_temp", mesh)
        bpy.context.collection.objects.link(temp_obj)

        try:
            # Select only the temp object
            bpy.ops.object.select_all(action='DESELECT')
            temp_obj.select_set(True)
            bpy.context.view_layer.objects.active = temp_obj

            # Use Blender's OBJ exporter
            bpy.ops.wm.obj_export(
                filepath=filepath,
                export_selected_objects=True,
                export_triangulated_mesh=False,  # Already triangulated
                export_normals=True,
                export_uv=True,
                export_materials=False,
                apply_modifiers=False,  # Already applied
                global_scale=1.0,
                forward_axis='NEGATIVE_Z',
                up_axis='Y',
            )

            return (True, "Export successful")

        finally:
            # Clean up temp object
            bpy.data.objects.remove(temp_obj)
            if apply_modifiers:
                obj_eval.to_mesh_clear()
            else:
                bpy.data.meshes.remove(mesh)

    except Exception as e:
        return (False, f"Error exporting mesh: {str(e)}")

    finally:
        # Restore original selection
        bpy.ops.object.select_all(action='DESELECT')
        for o in original_selection:
            if o:
                o.select_set(True)
        if original_active:
            bpy.context.view_layer.objects.active = original_active


def get_object_bounds(obj):
    """Get the world-space bounding box of an object."""
    bbox = [obj.matrix_world @ Vector(corner) for corner in obj.bound_box]
    min_corner = [min(v[i] for v in bbox) for i in range(3)]
    max_corner = [max(v[i] for v in bbox) for i in range(3)]
    return {"min": min_corner, "max": max_corner}


def matrix_to_list(matrix):
    """Convert a Blender matrix to a column-major list for JSON."""
    # Blender uses row-major, we need column-major for the spec
    return [
        matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0],
        matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1],
        matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2],
        matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3],
    ]


# Import Vector for bounds calculation
try:
    from mathutils import Vector
except ImportError:
    pass  # Will be available when running in Blender


# ==============================================================================
# Addon Preferences
# ==============================================================================

class VGEO_AddonPreferences(AddonPreferences):
    """Addon preferences for VGEO tools."""
    bl_idname = __name__

    build_path: StringProperty(
        name="vgeo_build Path",
        description="Path to the vgeo_build executable",
        default="",
        subtype='FILE_PATH',
    )

    viewer_path: StringProperty(
        name="vgeo_viewer Path",
        description="Path to the vgeo_viewer executable",
        default="",
        subtype='FILE_PATH',
    )

    default_max_verts: IntProperty(
        name="Max Vertices per Meshlet",
        description="Default maximum vertices per meshlet",
        default=64,
        min=32,
        max=256,
    )

    default_max_tris: IntProperty(
        name="Max Triangles per Meshlet",
        description="Default maximum triangles per meshlet",
        default=124,
        min=32,
        max=256,
    )

    default_compress: BoolProperty(
        name="Enable Compression",
        description="Enable LZ4 compression by default",
        default=True,
    )

    default_quantize: BoolProperty(
        name="Enable Quantization",
        description="Enable position quantization by default",
        default=False,
    )

    quantize_bits: EnumProperty(
        name="Quantization Bits",
        description="Bits per position component for quantization",
        items=[
            ('16', "16-bit", "6 bytes per vertex"),
            ('21', "21-bit", "8 bytes per vertex, higher precision"),
        ],
        default='16',
    )

    auto_export: BoolProperty(
        name="Auto Export on Preview",
        description="Automatically export when starting preview",
        default=True,
    )

    keep_temp_files: BoolProperty(
        name="Keep Temporary Files",
        description="Don't delete temporary OBJ files after conversion (for debugging)",
        default=False,
    )

    def draw(self, context):
        layout = self.layout

        # Executable paths
        box = layout.box()
        box.label(text="Executable Paths", icon='FILE_FOLDER')

        row = box.row()
        row.prop(self, "build_path")
        op = row.operator("vgeo.find_executable", text="", icon='VIEWZOOM')
        op.executable_name = "vgeo_build"

        # Show status
        build_exe = find_executable("vgeo_build", self)
        if build_exe:
            box.label(text=f"Found: {build_exe}", icon='CHECKMARK')
        else:
            box.label(text="vgeo_build not found", icon='ERROR')

        row = box.row()
        row.prop(self, "viewer_path")
        op = row.operator("vgeo.find_executable", text="", icon='VIEWZOOM')
        op.executable_name = "vgeo_viewer"

        viewer_exe = find_executable("vgeo_viewer", self)
        if viewer_exe:
            box.label(text=f"Found: {viewer_exe}", icon='CHECKMARK')
        else:
            box.label(text="vgeo_viewer not found", icon='ERROR')

        # Default export settings
        box = layout.box()
        box.label(text="Default Export Settings", icon='EXPORT')

        row = box.row()
        row.prop(self, "default_max_verts")
        row.prop(self, "default_max_tris")

        row = box.row()
        row.prop(self, "default_compress")
        row.prop(self, "default_quantize")

        if self.default_quantize:
            box.prop(self, "quantize_bits")

        # Behavior settings
        box = layout.box()
        box.label(text="Behavior", icon='PREFERENCES')
        box.prop(self, "auto_export")
        box.prop(self, "keep_temp_files")


class VGEO_OT_FindExecutable(Operator):
    """Search for VGEO executable"""
    bl_idname = "vgeo.find_executable"
    bl_label = "Find Executable"
    bl_options = {'REGISTER', 'INTERNAL'}

    executable_name: StringProperty()

    def execute(self, context):
        prefs = get_preferences()
        exe = find_executable(self.executable_name)

        if exe:
            if self.executable_name == "vgeo_build":
                prefs.build_path = exe
            else:
                prefs.viewer_path = exe
            self.report({'INFO'}, f"Found {self.executable_name} at: {exe}")
        else:
            self.report({'WARNING'}, f"Could not find {self.executable_name}")

        return {'FINISHED'}


# ==============================================================================
# Scene Properties
# ==============================================================================

class VGEO_SceneProperties(PropertyGroup):
    """Scene-level VGEO properties."""

    error_threshold: FloatProperty(
        name="Error Threshold",
        description="Screen-space error threshold in pixels for LOD selection",
        default=1.0,
        min=0.1,
        max=10.0,
        step=10,
        precision=2,
    )

    export_path: StringProperty(
        name="Export Path",
        description="Default export path for .vgeo files",
        default="//",
        subtype='DIR_PATH',
    )

    max_verts: IntProperty(
        name="Max Vertices",
        description="Maximum vertices per meshlet",
        default=64,
        min=32,
        max=256,
    )

    max_tris: IntProperty(
        name="Max Triangles",
        description="Maximum triangles per meshlet",
        default=124,
        min=32,
        max=256,
    )

    use_compression: BoolProperty(
        name="Compression",
        description="Enable LZ4 compression",
        default=True,
    )

    use_quantization: BoolProperty(
        name="Quantization",
        description="Enable position quantization",
        default=False,
    )

    quantize_bits: EnumProperty(
        name="Quantize Bits",
        description="Bits per position component",
        items=[
            ('16', "16-bit", "6 bytes per vertex"),
            ('21', "21-bit", "8 bytes per vertex"),
        ],
        default='16',
    )

    apply_modifiers: BoolProperty(
        name="Apply Modifiers",
        description="Apply modifiers before export",
        default=True,
    )


# ==============================================================================
# Export Operator
# ==============================================================================

class VGEO_OT_Export(Operator):
    """Export selected mesh objects to .vgeo format"""
    bl_idname = "vgeo.export"
    bl_label = "Export to VGEO"
    bl_description = "Export selected mesh objects to .vgeo format"
    bl_options = {'REGISTER', 'UNDO'}

    filepath: StringProperty(
        name="File Path",
        description="Path for the exported .vgeo file",
        subtype='FILE_PATH',
    )

    filename_ext = ".vgeo"

    filter_glob: StringProperty(
        default="*.vgeo",
        options={'HIDDEN'},
    )

    # Export options
    max_verts: IntProperty(
        name="Max Vertices",
        description="Maximum vertices per meshlet",
        default=64,
        min=32,
        max=256,
    )

    max_tris: IntProperty(
        name="Max Triangles",
        description="Maximum triangles per meshlet",
        default=124,
        min=32,
        max=256,
    )

    use_compression: BoolProperty(
        name="Compression",
        description="Enable LZ4 compression",
        default=True,
    )

    use_quantization: BoolProperty(
        name="Quantization",
        description="Enable position quantization",
        default=False,
    )

    quantize_bits: EnumProperty(
        name="Quantize Bits",
        items=[
            ('16', "16-bit", ""),
            ('21', "21-bit", ""),
        ],
        default='16',
    )

    apply_modifiers: BoolProperty(
        name="Apply Modifiers",
        description="Apply modifiers before export",
        default=True,
    )

    export_selected: BoolProperty(
        name="Selected Only",
        description="Export only selected objects",
        default=True,
    )

    batch_export: BoolProperty(
        name="Batch Export",
        description="Export each object to a separate file",
        default=False,
    )

    @classmethod
    def poll(cls, context):
        return context.selected_objects and any(
            obj.type == 'MESH' for obj in context.selected_objects
        )

    def invoke(self, context, event):
        # Initialize from scene properties
        scene_props = context.scene.vgeo
        self.max_verts = scene_props.max_verts
        self.max_tris = scene_props.max_tris
        self.use_compression = scene_props.use_compression
        self.use_quantization = scene_props.use_quantization
        self.quantize_bits = scene_props.quantize_bits
        self.apply_modifiers = scene_props.apply_modifiers

        # Set default filename from active object
        if context.active_object and context.active_object.type == 'MESH':
            self.filepath = context.active_object.name + ".vgeo"
        else:
            self.filepath = "export.vgeo"

        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def draw(self, context):
        layout = self.layout

        layout.prop(self, "export_selected")
        layout.prop(self, "batch_export")
        layout.prop(self, "apply_modifiers")

        layout.separator()
        layout.label(text="Meshlet Settings:")
        row = layout.row()
        row.prop(self, "max_verts")
        row.prop(self, "max_tris")

        layout.separator()
        layout.label(text="Optimization:")
        layout.prop(self, "use_compression")
        layout.prop(self, "use_quantization")
        if self.use_quantization:
            layout.prop(self, "quantize_bits")

    def execute(self, context):
        prefs = get_preferences()

        # Check for vgeo_build
        build_exe = find_executable("vgeo_build", prefs)
        if not build_exe:
            self.report({'ERROR'}, "vgeo_build not found. Please set the path in addon preferences.")
            return {'CANCELLED'}

        # Get mesh objects to export
        if self.export_selected:
            objects = [obj for obj in context.selected_objects if obj.type == 'MESH']
        else:
            objects = [obj for obj in context.view_layer.objects if obj.type == 'MESH' and obj.visible_get()]

        if not objects:
            self.report({'WARNING'}, "No mesh objects to export")
            return {'CANCELLED'}

        # Build options
        options = {
            "max_verts": self.max_verts,
            "max_tris": self.max_tris,
            "compress": self.use_compression,
            "quantize": self.use_quantization,
            "quantize_bits": int(self.quantize_bits) if self.use_quantization else None,
        }

        temp_dir = tempfile.mkdtemp(prefix="vgeo_")
        exported_count = 0
        failed_count = 0

        try:
            if self.batch_export:
                # Export each object to separate file
                base_dir = os.path.dirname(self.filepath)

                for obj in objects:
                    obj_path = os.path.join(base_dir, obj.name + ".vgeo")
                    temp_obj = os.path.join(temp_dir, obj.name + ".obj")

                    # Export to OBJ
                    success, msg = export_mesh_to_obj(
                        obj, temp_obj,
                        apply_modifiers=self.apply_modifiers,
                        triangulate=True
                    )

                    if not success:
                        self.report({'WARNING'}, f"Failed to export {obj.name}: {msg}")
                        failed_count += 1
                        continue

                    # Convert to VGEO
                    success, msg, stats = run_vgeo_build(temp_obj, obj_path, options)

                    if success:
                        exported_count += 1
                    else:
                        self.report({'WARNING'}, f"Failed to convert {obj.name}: {msg}")
                        failed_count += 1
            else:
                # Export all objects to single file
                # First combine into a single OBJ
                temp_obj = os.path.join(temp_dir, "combined.obj")

                if len(objects) == 1:
                    success, msg = export_mesh_to_obj(
                        objects[0], temp_obj,
                        apply_modifiers=self.apply_modifiers,
                        triangulate=True
                    )
                else:
                    # Join meshes temporarily
                    success, msg = self._export_combined(context, objects, temp_obj)

                if not success:
                    self.report({'ERROR'}, f"Failed to export: {msg}")
                    return {'CANCELLED'}

                # Convert to VGEO
                success, msg, stats = run_vgeo_build(temp_obj, self.filepath, options)

                if success:
                    exported_count = len(objects)
                    if stats:
                        self.report({'INFO'}, f"Exported {exported_count} objects: {stats}")
                    else:
                        self.report({'INFO'}, f"Exported {exported_count} objects to {self.filepath}")
                else:
                    self.report({'ERROR'}, msg)
                    return {'CANCELLED'}

        finally:
            # Clean up temp directory
            if not prefs.keep_temp_files:
                shutil.rmtree(temp_dir, ignore_errors=True)
            else:
                self.report({'INFO'}, f"Temp files kept at: {temp_dir}")

        if self.batch_export:
            self.report({'INFO'}, f"Exported {exported_count} objects, {failed_count} failed")

        return {'FINISHED'}

    def _export_combined(self, context, objects, filepath):
        """Export multiple objects combined into a single OBJ."""
        try:
            # Store original selection
            original_active = context.view_layer.objects.active
            original_selection = [o for o in context.selected_objects]

            # Deselect all and select target objects
            bpy.ops.object.select_all(action='DESELECT')
            for obj in objects:
                obj.select_set(True)
            context.view_layer.objects.active = objects[0]

            # Export using Blender's OBJ exporter
            bpy.ops.wm.obj_export(
                filepath=filepath,
                export_selected_objects=True,
                export_triangulated_mesh=True,
                export_normals=True,
                export_uv=True,
                export_materials=False,
                apply_modifiers=self.apply_modifiers,
                global_scale=1.0,
                forward_axis='NEGATIVE_Z',
                up_axis='Y',
            )

            # Restore selection
            bpy.ops.object.select_all(action='DESELECT')
            for o in original_selection:
                if o:
                    o.select_set(True)
            if original_active:
                context.view_layer.objects.active = original_active

            return (True, "Export successful")

        except Exception as e:
            return (False, str(e))


# ==============================================================================
# Preview Operator
# ==============================================================================

class VGEO_OT_Preview(Operator):
    """Export and preview selected mesh in VGEO viewer"""
    bl_idname = "vgeo.preview"
    bl_label = "Preview in VGEO Viewer"
    bl_description = "Export selection and launch VGEO viewer"
    bl_options = {'REGISTER'}

    _process = None

    @classmethod
    def poll(cls, context):
        return context.selected_objects and any(
            obj.type == 'MESH' for obj in context.selected_objects
        )

    def execute(self, context):
        prefs = get_preferences()
        scene_props = context.scene.vgeo

        # Check for executables
        build_exe = find_executable("vgeo_build", prefs)
        viewer_exe = find_executable("vgeo_viewer", prefs)

        if not build_exe:
            self.report({'ERROR'}, "vgeo_build not found. Please set the path in addon preferences.")
            return {'CANCELLED'}

        if not viewer_exe:
            self.report({'ERROR'}, "vgeo_viewer not found. Please set the path in addon preferences.")
            return {'CANCELLED'}

        # Get mesh objects
        objects = [obj for obj in context.selected_objects if obj.type == 'MESH']
        if not objects:
            self.report({'WARNING'}, "No mesh objects selected")
            return {'CANCELLED'}

        # Create temp directory for preview files
        temp_dir = tempfile.mkdtemp(prefix="vgeo_preview_")
        temp_obj = os.path.join(temp_dir, "preview.obj")
        temp_vgeo = os.path.join(temp_dir, "preview.vgeo")

        try:
            # Export to OBJ
            if len(objects) == 1:
                success, msg = export_mesh_to_obj(
                    objects[0], temp_obj,
                    apply_modifiers=scene_props.apply_modifiers,
                    triangulate=True
                )
            else:
                # Use combined export
                bpy.ops.object.select_all(action='DESELECT')
                for obj in objects:
                    obj.select_set(True)
                context.view_layer.objects.active = objects[0]

                bpy.ops.wm.obj_export(
                    filepath=temp_obj,
                    export_selected_objects=True,
                    export_triangulated_mesh=True,
                    export_normals=True,
                    export_uv=True,
                    export_materials=False,
                    apply_modifiers=scene_props.apply_modifiers,
                )
                success, msg = True, "OK"

            if not success:
                self.report({'ERROR'}, f"Failed to export: {msg}")
                return {'CANCELLED'}

            # Build options from scene properties
            options = {
                "max_verts": scene_props.max_verts,
                "max_tris": scene_props.max_tris,
                "compress": scene_props.use_compression,
                "quantize": scene_props.use_quantization,
                "quantize_bits": int(scene_props.quantize_bits) if scene_props.use_quantization else None,
            }

            # Convert to VGEO
            success, msg, stats = run_vgeo_build(temp_obj, temp_vgeo, options)

            if not success:
                self.report({'ERROR'}, f"Failed to build VGEO: {msg}")
                return {'CANCELLED'}

            # Launch viewer (non-blocking)
            cmd = [viewer_exe, temp_vgeo]

            # Pass error threshold if supported
            cmd.extend(["--error-threshold", str(scene_props.error_threshold)])

            try:
                # Use Popen for non-blocking execution
                VGEO_OT_Preview._process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                self.report({'INFO'}, "VGEO viewer launched")

            except Exception as e:
                self.report({'ERROR'}, f"Failed to launch viewer: {str(e)}")
                return {'CANCELLED'}

        except Exception as e:
            self.report({'ERROR'}, f"Preview failed: {str(e)}")
            # Clean up on error
            shutil.rmtree(temp_dir, ignore_errors=True)
            return {'CANCELLED'}

        # Note: Don't clean up temp_dir here - viewer needs the files
        # They will be cleaned up when Blender closes or on next preview

        return {'FINISHED'}


# ==============================================================================
# Quick Preview Operator
# ==============================================================================

class VGEO_OT_QuickPreview(Operator):
    """Quick one-click preview of selected mesh"""
    bl_idname = "vgeo.quick_preview"
    bl_label = "Quick Preview"
    bl_description = "Instantly preview selected mesh in VGEO viewer"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return context.active_object and context.active_object.type == 'MESH'

    def execute(self, context):
        # Simply call the preview operator
        return bpy.ops.vgeo.preview()


# ==============================================================================
# Scene Export Operator
# ==============================================================================

class VGEO_OT_ExportScene(Operator):
    """Export all visible meshes as a .vscene manifest"""
    bl_idname = "vgeo.export_scene"
    bl_label = "Export Scene to VSCENE"
    bl_description = "Export visible meshes and transforms as .vscene manifest"
    bl_options = {'REGISTER', 'UNDO'}

    filepath: StringProperty(
        name="File Path",
        description="Path for the .vscene file",
        subtype='FILE_PATH',
    )

    filename_ext = ".vscene"

    filter_glob: StringProperty(
        default="*.vscene",
        options={'HIDDEN'},
    )

    export_hidden: BoolProperty(
        name="Include Hidden",
        description="Include hidden objects",
        default=False,
    )

    use_instances: BoolProperty(
        name="Detect Instances",
        description="Detect and reuse meshes with same data",
        default=True,
    )

    include_camera: BoolProperty(
        name="Include Camera",
        description="Include active camera in scene",
        default=True,
    )

    @classmethod
    def poll(cls, context):
        return any(obj.type == 'MESH' for obj in context.view_layer.objects)

    def invoke(self, context, event):
        if bpy.data.filepath:
            base = os.path.splitext(os.path.basename(bpy.data.filepath))[0]
            self.filepath = base + ".vscene"
        else:
            self.filepath = "scene.vscene"

        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "export_hidden")
        layout.prop(self, "use_instances")
        layout.prop(self, "include_camera")

    def execute(self, context):
        prefs = get_preferences()
        scene_props = context.scene.vgeo

        # Check for vgeo_build
        build_exe = find_executable("vgeo_build", prefs)
        if not build_exe:
            self.report({'ERROR'}, "vgeo_build not found")
            return {'CANCELLED'}

        # Gather mesh objects
        if self.export_hidden:
            objects = [obj for obj in context.view_layer.objects if obj.type == 'MESH']
        else:
            objects = [obj for obj in context.view_layer.objects if obj.type == 'MESH' and obj.visible_get()]

        if not objects:
            self.report({'WARNING'}, "No mesh objects to export")
            return {'CANCELLED'}

        # Determine output directory
        scene_dir = os.path.dirname(self.filepath)
        meshes_dir = os.path.join(scene_dir, "meshes")
        os.makedirs(meshes_dir, exist_ok=True)

        # Build asset list (detect shared mesh data for instancing)
        assets = {}  # mesh_data -> asset_info
        instances = []

        for obj in objects:
            mesh_key = obj.data.name if self.use_instances else obj.name

            if mesh_key not in assets:
                # New unique asset
                asset_id = mesh_key.replace(" ", "_").lower()
                asset_path = f"meshes/{asset_id}.vgeo"
                full_path = os.path.join(scene_dir, asset_path)

                # Export mesh
                temp_obj = os.path.join(tempfile.gettempdir(), f"{asset_id}.obj")

                success, msg = export_mesh_to_obj(
                    obj, temp_obj,
                    apply_modifiers=scene_props.apply_modifiers,
                    triangulate=True
                )

                if not success:
                    self.report({'WARNING'}, f"Failed to export {obj.name}: {msg}")
                    continue

                options = {
                    "max_verts": scene_props.max_verts,
                    "max_tris": scene_props.max_tris,
                    "compress": scene_props.use_compression,
                    "quantize": scene_props.use_quantization,
                }

                success, msg, stats = run_vgeo_build(temp_obj, full_path, options)

                if not success:
                    self.report({'WARNING'}, f"Failed to convert {obj.name}: {msg}")
                    continue

                # Clean up temp file
                try:
                    os.remove(temp_obj)
                except:
                    pass

                bounds = get_object_bounds(obj)

                assets[mesh_key] = {
                    "id": asset_id,
                    "name": obj.data.name,
                    "path": asset_path,
                    "bounds": bounds,
                }

            # Add instance
            if mesh_key in assets:
                instance_id = f"{obj.name.replace(' ', '_').lower()}"
                instances.append({
                    "id": instance_id,
                    "asset": assets[mesh_key]["id"],
                    "transform": matrix_to_list(obj.matrix_world),
                    "visible": obj.visible_get() if not self.export_hidden else True,
                })

        # Build camera info
        camera_info = None
        if self.include_camera and context.scene.camera:
            cam = context.scene.camera
            cam_data = cam.data

            # Get camera vectors
            pos = list(cam.location)

            # Calculate target (point in front of camera)
            forward = cam.matrix_world.to_quaternion() @ Vector((0, 0, -1))
            target = list(cam.location + forward * 10)

            up = list(cam.matrix_world.to_quaternion() @ Vector((0, 1, 0)))

            camera_info = {
                "type": "perspective" if cam_data.type == 'PERSP' else "orthographic",
                "position": pos,
                "target": target,
                "up": up,
                "fov": math.degrees(cam_data.angle) if cam_data.type == 'PERSP' else 45,
                "near": cam_data.clip_start,
                "far": cam_data.clip_end,
            }

            if cam_data.type == 'ORTHO':
                camera_info["ortho_scale"] = cam_data.ortho_scale

        # Build scene manifest
        scene_data = {
            "version": "0.1.0",
            "generator": f"vgeo-blender-addon/{'.'.join(map(str, bl_info['version']))}",
            "created": datetime.utcnow().isoformat() + "Z",
            "settings": {
                "units": "meters",
                "up_axis": "Z",
                "error_threshold": scene_props.error_threshold,
            },
            "assets": list(assets.values()),
            "instances": instances,
        }

        if camera_info:
            scene_data["camera"] = camera_info

        # Write JSON
        try:
            with open(self.filepath, 'w', encoding='utf-8') as f:
                json.dump(scene_data, f, indent=2)

            self.report({'INFO'}, f"Exported scene with {len(assets)} assets and {len(instances)} instances")
            return {'FINISHED'}

        except Exception as e:
            self.report({'ERROR'}, f"Failed to write scene file: {str(e)}")
            return {'CANCELLED'}


# ==============================================================================
# View Scene Operator
# ==============================================================================

class VGEO_OT_ViewScene(Operator):
    """View an existing .vscene file in the VGEO viewer"""
    bl_idname = "vgeo.view_scene"
    bl_label = "View VSCENE"
    bl_description = "Open a .vscene file in the VGEO viewer"
    bl_options = {'REGISTER'}

    filepath: StringProperty(
        name="File Path",
        subtype='FILE_PATH',
    )

    filter_glob: StringProperty(
        default="*.vscene;*.vgeo",
        options={'HIDDEN'},
    )

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        prefs = get_preferences()
        viewer_exe = find_executable("vgeo_viewer", prefs)

        if not viewer_exe:
            self.report({'ERROR'}, "vgeo_viewer not found")
            return {'CANCELLED'}

        if not os.path.isfile(self.filepath):
            self.report({'ERROR'}, f"File not found: {self.filepath}")
            return {'CANCELLED'}

        try:
            subprocess.Popen(
                [viewer_exe, self.filepath],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            self.report({'INFO'}, "VGEO viewer launched")
            return {'FINISHED'}

        except Exception as e:
            self.report({'ERROR'}, f"Failed to launch viewer: {str(e)}")
            return {'CANCELLED'}


# ==============================================================================
# UI Panel
# ==============================================================================

class VGEO_PT_Panel(Panel):
    """Main VGEO panel in the 3D viewport sidebar"""
    bl_label = "VGEO"
    bl_idname = "VGEO_PT_panel"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "VGEO"

    def draw(self, context):
        layout = self.layout
        scene_props = context.scene.vgeo
        prefs = get_preferences()

        # Status section
        prefs_ok = True
        if not find_executable("vgeo_build", prefs):
            box = layout.box()
            box.alert = True
            box.label(text="vgeo_build not found!", icon='ERROR')
            box.operator("preferences.addon_show", text="Open Preferences").module = __name__
            prefs_ok = False

        if not find_executable("vgeo_viewer", prefs):
            box = layout.box()
            box.alert = True
            box.label(text="vgeo_viewer not found!", icon='ERROR')
            prefs_ok = False

        # Quick actions
        box = layout.box()
        box.label(text="Quick Actions", icon='PLAY')

        row = box.row(align=True)
        row.scale_y = 1.5
        row.operator("vgeo.quick_preview", text="Preview", icon='RESTRICT_VIEW_OFF')
        row.enabled = prefs_ok

        # Export section
        box = layout.box()
        box.label(text="Export", icon='EXPORT')

        col = box.column(align=True)
        col.operator("vgeo.export", text="Export Selected", icon='MESH_DATA')
        col.operator("vgeo.export_scene", text="Export Scene", icon='SCENE_DATA')
        col.enabled = prefs_ok

        # View existing files
        box.separator()
        box.operator("vgeo.view_scene", text="Open File in Viewer", icon='FILE')

        # Settings section
        box = layout.box()
        box.label(text="Export Settings", icon='PREFERENCES')

        row = box.row()
        row.prop(scene_props, "max_verts")
        row.prop(scene_props, "max_tris")

        box.prop(scene_props, "use_compression")
        box.prop(scene_props, "use_quantization")

        if scene_props.use_quantization:
            box.prop(scene_props, "quantize_bits")

        box.prop(scene_props, "apply_modifiers")

        # LOD Settings
        box = layout.box()
        box.label(text="LOD Settings", icon='MOD_DECIM')
        box.prop(scene_props, "error_threshold", slider=True)

        # Selection info
        mesh_count = sum(1 for obj in context.selected_objects if obj.type == 'MESH')
        if mesh_count > 0:
            box = layout.box()
            box.label(text=f"Selected: {mesh_count} mesh(es)", icon='INFO')

            # Show basic stats
            total_verts = 0
            total_tris = 0
            for obj in context.selected_objects:
                if obj.type == 'MESH':
                    total_verts += len(obj.data.vertices)
                    total_tris += len(obj.data.polygons)

            box.label(text=f"Vertices: {total_verts:,}")
            box.label(text=f"Faces: {total_tris:,}")


class VGEO_PT_AdvancedPanel(Panel):
    """Advanced options sub-panel"""
    bl_label = "Advanced"
    bl_idname = "VGEO_PT_advanced"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "VGEO"
    bl_parent_id = "VGEO_PT_panel"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        scene_props = context.scene.vgeo

        layout.prop(scene_props, "export_path")

        layout.separator()
        layout.operator("preferences.addon_show", text="Addon Preferences").module = __name__


# ==============================================================================
# File Menu Integration
# ==============================================================================

def menu_func_export(self, context):
    """Add VGEO export to File > Export menu."""
    self.layout.operator(VGEO_OT_Export.bl_idname, text="VGEO Mesh (.vgeo)")
    self.layout.operator(VGEO_OT_ExportScene.bl_idname, text="VGEO Scene (.vscene)")


def menu_func_import(self, context):
    """Add VGEO view to File > Import menu."""
    self.layout.operator(VGEO_OT_ViewScene.bl_idname, text="View VGEO File")


# ==============================================================================
# Registration
# ==============================================================================

classes = (
    VGEO_AddonPreferences,
    VGEO_OT_FindExecutable,
    VGEO_SceneProperties,
    VGEO_OT_Export,
    VGEO_OT_Preview,
    VGEO_OT_QuickPreview,
    VGEO_OT_ExportScene,
    VGEO_OT_ViewScene,
    VGEO_PT_Panel,
    VGEO_PT_AdvancedPanel,
)


def register():
    """Register all classes and properties."""
    for cls in classes:
        bpy.utils.register_class(cls)

    # Register scene properties
    bpy.types.Scene.vgeo = PointerProperty(type=VGEO_SceneProperties)

    # Add menu items
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)

    print(f"VGEO Addon v{'.'.join(map(str, bl_info['version']))} registered")


def unregister():
    """Unregister all classes and properties."""
    # Remove menu items
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)

    # Unregister scene properties
    del bpy.types.Scene.vgeo

    # Unregister classes
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)

    print("VGEO Addon unregistered")


if __name__ == "__main__":
    register()
