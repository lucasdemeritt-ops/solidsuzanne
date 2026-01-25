"""
VGEO Render Engine
Custom RenderEngine for virtualized geometry viewport rendering
"""

import bpy
from bpy.types import RenderEngine
import gpu
from gpu_extras.presets import draw_texture_2d
import numpy as np

# Try to import native module
try:
    import vgeo_native
    NATIVE_AVAILABLE = True
except ImportError:
    NATIVE_AVAILABLE = False


class VGEORenderEngine(RenderEngine):
    """VGEO Viewport Render Engine"""

    bl_idname = "VGEO"
    bl_label = "VGEO"
    bl_use_preview = False  # No material preview for now
    bl_use_eevee_viewport = False
    bl_use_gpu_context = True

    def __init__(self):
        super().__init__()
        self.scene = None
        self.camera = None
        self._object_map = {}  # Blender object -> VGEO object ID

        if NATIVE_AVAILABLE:
            self.scene = vgeo_native.Scene()
            self.camera = vgeo_native.Camera()
            print("VGEO Engine: Initialized")
        else:
            print("VGEO Engine: Native module not available")

    def __del__(self):
        if hasattr(self, 'scene') and self.scene:
            self.scene.clear()
        print("VGEO Engine: Destroyed")

    # =========================================================================
    # Viewport Rendering
    # =========================================================================

    def view_update(self, context, depsgraph):
        """Called when the scene or view changes"""
        if not NATIVE_AVAILABLE or not self.scene:
            return

        # Sync scene objects
        self._sync_scene(depsgraph)

    def view_draw(self, context, depsgraph):
        """Called to draw the viewport"""
        if not NATIVE_AVAILABLE or not self.scene:
            self._draw_fallback(context)
            return

        # Update camera from Blender's view
        self._update_camera(context)

        # For now, just draw a placeholder
        # TODO: Implement actual Vulkan rendering + blit
        self._draw_placeholder(context)

    def _sync_scene(self, depsgraph):
        """Synchronize Blender scene with VGEO scene"""
        # Track which objects we've seen
        seen_objects = set()

        for obj in depsgraph.objects:
            if obj.type != 'MESH':
                continue

            # Check if this object has a vgeo_path custom property
            vgeo_path = obj.get("vgeo_path", None)
            if not vgeo_path:
                continue

            seen_objects.add(obj.name)

            # Get transform as flat 4x4 matrix
            transform = np.array(obj.matrix_world, dtype=np.float32).T.flatten()

            if obj.name in self._object_map:
                # Update existing object's transform
                vgeo_id = self._object_map[obj.name]
                self.scene.set_transform(vgeo_id, transform)
            else:
                # Add new object
                vgeo_id = self.scene.add_object_from_file(vgeo_path, transform, obj.name)
                if vgeo_id >= 0:
                    self._object_map[obj.name] = vgeo_id
                    print(f"VGEO: Added object '{obj.name}' (ID: {vgeo_id})")

        # Remove objects that no longer exist
        for obj_name in list(self._object_map.keys()):
            if obj_name not in seen_objects:
                vgeo_id = self._object_map.pop(obj_name)
                self.scene.remove_object(vgeo_id)
                print(f"VGEO: Removed object '{obj_name}'")

    def _update_camera(self, context):
        """Update VGEO camera from Blender's 3D view"""
        if not self.camera:
            return

        region = context.region
        rv3d = context.region_data

        # Get view matrix and extract camera position
        view_matrix = rv3d.view_matrix.inverted()
        cam_pos = view_matrix.translation

        # Camera looks down -Z in view space
        target = cam_pos - view_matrix.col[2].xyz * 10.0

        self.camera.set_position(cam_pos.x, cam_pos.y, cam_pos.z)
        self.camera.set_target(target.x, target.y, target.z)
        self.camera.aspect = region.width / max(region.height, 1)
        self.camera.fov = 45.0  # TODO: Get actual FOV from view
        self.camera.update()

    def _draw_placeholder(self, context):
        """Draw a placeholder indicating VGEO is active"""
        # Get region dimensions
        region = context.region

        # Draw text overlay
        gpu.state.blend_set('ALPHA')

        # Create a simple colored quad to show the engine is active
        from gpu_extras.batch import batch_for_shader
        shader = gpu.shader.from_builtin('UNIFORM_COLOR')

        # Draw a small indicator in the corner
        vertices = [
            (10, 10), (100, 10), (100, 30), (10, 30)
        ]
        shader.bind()
        shader.uniform_float("color", (0.2, 0.5, 0.8, 0.8))

        batch = batch_for_shader(shader, 'TRI_FAN', {"pos": vertices})
        batch.draw(shader)

        # Draw scene stats
        if self.scene:
            stats = self.scene.get_stats()
            # Note: Would need blf for text drawing
            pass

        gpu.state.blend_set('NONE')

    def _draw_fallback(self, context):
        """Draw fallback when native module not available"""
        from gpu_extras.batch import batch_for_shader

        gpu.state.blend_set('ALPHA')
        shader = gpu.shader.from_builtin('UNIFORM_COLOR')

        # Red indicator for error state
        vertices = [
            (10, 10), (200, 10), (200, 30), (10, 30)
        ]
        shader.bind()
        shader.uniform_float("color", (0.8, 0.2, 0.2, 0.8))

        batch = batch_for_shader(shader, 'TRI_FAN', {"pos": vertices})
        batch.draw(shader)

        gpu.state.blend_set('NONE')

    # =========================================================================
    # Final Rendering (for F12 / Cycles handoff)
    # =========================================================================

    def render(self, depsgraph):
        """Called for final render (F12)"""
        scene = depsgraph.scene
        scale = scene.render.resolution_percentage / 100.0
        self.size_x = int(scene.render.resolution_x * scale)
        self.size_y = int(scene.render.resolution_y * scale)

        # For final render, we'll query geometry and pass to Cycles
        # For now, just fill with a test pattern
        self._render_test_pattern()

    def _render_test_pattern(self):
        """Render a test pattern for final render"""
        pixel_count = self.size_x * self.size_y

        # Create gradient test pattern
        rect = np.zeros((pixel_count, 4), dtype=np.float32)
        for y in range(self.size_y):
            for x in range(self.size_x):
                i = y * self.size_x + x
                rect[i] = [
                    x / self.size_x,  # R
                    y / self.size_y,  # G
                    0.5,              # B
                    1.0               # A
                ]

        # Update render result
        result = self.begin_result(0, 0, self.size_x, self.size_y)
        layer = result.layers[0].passes["Combined"]
        layer.rect = rect.flatten()
        self.end_result(result)


# Registration
def get_panels():
    """Get panels to exclude from VGEO engine"""
    exclude_panels = {
        'VIEWLAYER_PT_filter',
        'VIEWLAYER_PT_layer_passes',
    }

    panels = []
    for panel in bpy.types.Panel.__subclasses__():
        if hasattr(panel, 'COMPAT_ENGINES') and 'BLENDER_RENDER' in panel.COMPAT_ENGINES:
            if panel.__name__ not in exclude_panels:
                panels.append(panel)

    return panels


def register():
    bpy.utils.register_class(VGEORenderEngine)

    # Add VGEO to compatible engines for relevant panels
    for panel in get_panels():
        panel.COMPAT_ENGINES.add('VGEO')

    print("VGEO Engine: Registered")


def unregister():
    # Remove VGEO from panel compatibility
    for panel in get_panels():
        if 'VGEO' in panel.COMPAT_ENGINES:
            panel.COMPAT_ENGINES.remove('VGEO')

    bpy.utils.unregister_class(VGEORenderEngine)
    print("VGEO Engine: Unregistered")
