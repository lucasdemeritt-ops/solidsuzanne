"""
VGEO Operators
Import/export and conversion operators for .vgeo files
"""

import bpy
from bpy.types import Operator
from bpy.props import StringProperty, BoolProperty
from bpy_extras.io_utils import ImportHelper, ExportHelper
import os
import subprocess

# Try to import native module
try:
    import vgeo_native
    NATIVE_AVAILABLE = True
except ImportError:
    NATIVE_AVAILABLE = False


def find_vgeo_build():
    """Locate the vgeo_build binary relative to the addon directory"""
    addon_dir = os.path.dirname(os.path.realpath(__file__))
    exe = "vgeo_build.exe" if os.name == "nt" else "vgeo_build"
    tool_dir = os.path.join(addon_dir, "..", "..", "build", "tools", "vgeo_build")
    candidates = [
        os.path.join(addon_dir, exe),
        os.path.join(tool_dir, exe),             # Single-config (Linux/macOS)
        os.path.join(tool_dir, "Release", exe),  # MSVC Release
        os.path.join(tool_dir, "Debug", exe),    # MSVC Debug
    ]
    for c in candidates:
        path = os.path.realpath(c)
        if os.path.exists(path):
            return path
    return None


def import_vgeo_file(operator, filepath):
    """Shared logic: load a .vgeo, create a proxy mesh, set custom properties."""
    if not NATIVE_AVAILABLE:
        operator.report({'ERROR'}, "VGEO native module not available")
        return {'CANCELLED'}

    scene = vgeo_native.Scene()
    obj_id = scene.add_object_from_file(filepath, None, "temp")

    if obj_id < 0:
        operator.report({'ERROR'}, f"Failed to load {filepath}")
        return {'CANCELLED'}

    bounds = scene.get_object_bounds(obj_id)
    stats = scene.get_stats()

    min_b, max_b = bounds
    size = [max_b[i] - min_b[i] for i in range(3)]
    center = [(max_b[i] + min_b[i]) / 2 for i in range(3)]

    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.active_object
    obj.scale = [max(s / 2, 0.01) for s in size]
    obj.location = center
    obj.name = os.path.basename(filepath).replace('.vgeo', '')
    obj["vgeo_path"] = filepath
    obj["vgeo_meshlets"] = stats.get('total_meshlets', 0)
    obj["vgeo_clusters"] = stats.get('total_clusters', 0)

    operator.report({'INFO'}, f"Loaded {obj.name} — {stats.get('total_meshlets', 0):,} meshlets")
    return {'FINISHED'}


class VGEO_OT_import(Operator, ImportHelper):
    """Import a .vgeo file into the scene"""
    # Note: not "vgeo.import" — "import" is a Python keyword, which would
    # make bpy.ops.vgeo.import(...) unwritable in scripts
    bl_idname = "vgeo.import_file"
    bl_label = "Import VGEO"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".vgeo"
    # Only .vgeo: the import path feeds the file straight to the native
    # loader, which reads the VGEO format only (convert glTF via vgeo_build)
    filter_glob: StringProperty(default="*.vgeo", options={'HIDDEN'})

    def execute(self, context):
        return import_vgeo_file(self, self.filepath)


class VGEO_OT_convert(Operator, ExportHelper):
    """Convert the active mesh to VGEO format and load it"""
    bl_idname = "vgeo.convert"
    bl_label = "Convert to VGEO"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".vgeo"
    filter_glob: StringProperty(default="*.vgeo", options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        return context.active_object and context.active_object.type == 'MESH'

    def execute(self, context):
        vgeo_build = find_vgeo_build()
        if not vgeo_build:
            self.report({'ERROR'}, "vgeo_build tool not found (build the project first)")
            return {'CANCELLED'}

        source_obj = context.active_object
        tmp_obj = self.filepath.replace('.vgeo', '_tmp.obj')

        # Isolate selection for export; always restore it, even when the
        # export or conversion fails partway through
        prev_active = context.view_layer.objects.active
        prev_selected = list(context.selected_objects)
        bpy.ops.object.select_all(action='DESELECT')
        source_obj.select_set(True)
        context.view_layer.objects.active = source_obj

        try:
            # Export OBJ — try Blender 4.x API first, fall back to legacy
            exported = False
            try:
                bpy.ops.wm.obj_export(
                    filepath=tmp_obj,
                    export_selected_objects=True,
                    export_normals=True,
                    export_uv=False,
                    export_materials=False,
                )
                exported = True
            except AttributeError:
                pass

            if not exported:
                try:
                    bpy.ops.export_scene.obj(
                        filepath=tmp_obj,
                        use_selection=True,
                        use_normals=True,
                        use_uvs=False,
                        use_materials=False,
                    )
                    exported = True
                except Exception as e:
                    self.report({'ERROR'}, f"OBJ export failed: {e}")
                    return {'CANCELLED'}
        finally:
            # Restore selection
            bpy.ops.object.select_all(action='DESELECT')
            for o in prev_selected:
                o.select_set(True)
            context.view_layer.objects.active = prev_active

        if not os.path.exists(tmp_obj):
            self.report({'ERROR'}, "OBJ export produced no file")
            return {'CANCELLED'}

        # Run vgeo_build; temp files are removed on every exit path
        print(f"VGEO: Converting {tmp_obj} -> {self.filepath}")
        try:
            result = subprocess.run(
                [vgeo_build, tmp_obj, self.filepath],
                capture_output=True, text=True, timeout=120
            )
        except subprocess.TimeoutExpired:
            self.report({'ERROR'}, "vgeo_build timed out")
            return {'CANCELLED'}
        finally:
            for ext in ['.obj', '.mtl']:
                tmp = tmp_obj.replace('.obj', ext)
                if os.path.exists(tmp):
                    try:
                        os.remove(tmp)
                    except Exception:
                        pass

        if result.returncode != 0:
            msg = result.stderr.strip()[:300] if result.stderr else "unknown error"
            self.report({'ERROR'}, f"Conversion failed: {msg}")
            print(f"VGEO: vgeo_build output:\n{result.stdout}\n{result.stderr}")
            return {'CANCELLED'}

        if not os.path.exists(self.filepath):
            self.report({'ERROR'}, "vgeo_build produced no output file")
            return {'CANCELLED'}

        print(f"VGEO: Conversion complete\n{result.stdout}")

        # Auto-import the result
        return import_vgeo_file(self, self.filepath)


class VGEO_OT_reload(Operator):
    """Reload the VGEO file for the selected object"""
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

        self.report({'INFO'}, f"Marked for reload: {vgeo_path}")
        return {'FINISHED'}


# Operator list
classes = [
    VGEO_OT_import,
    VGEO_OT_convert,
    VGEO_OT_reload,
]


def menu_func_import(self, context):
    self.layout.operator(VGEO_OT_import.bl_idname, text="VGEO (.vgeo)")


def menu_func_convert(self, context):
    self.layout.operator(VGEO_OT_convert.bl_idname, text="Convert Mesh to VGEO")


def register():
    for cls in classes:
        bpy.utils.register_class(cls)

    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_convert)
    print("VGEO Operators: Registered")


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_convert)
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)

    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)

    print("VGEO Operators: Unregistered")
