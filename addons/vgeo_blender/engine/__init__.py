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
        self.renderer = None
        self._object_map = {}  # Blender object -> VGEO object ID
        self._texture = None
        self._image = None  # Blender Image for pixel transfer
        self._last_width = 0
        self._last_height = 0
        self._needs_upload = False

        if NATIVE_AVAILABLE:
            self.scene = vgeo_native.Scene()
            self.camera = vgeo_native.Camera()
            self.renderer = vgeo_native.Renderer()
            print("VGEO Engine: Initialized")
        else:
            print("VGEO Engine: Native module not available")

    def __del__(self):
        if hasattr(self, 'renderer') and self.renderer:
            self.renderer.destroy()
        if hasattr(self, 'scene') and self.scene:
            self.scene.clear()
        # Clean up viewport buffer image
        if hasattr(self, '_image') and self._image:
            try:
                if self._image.name in bpy.data.images:
                    bpy.data.images.remove(self._image)
            except:
                pass
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

        region = context.region
        width = region.width
        height = region.height

        # Initialize or resize renderer as needed
        if not self.renderer.is_initialized:
            if not self.renderer.init(width, height):
                print("VGEO: Failed to initialize renderer")
                self._draw_fallback(context)
                return
            self._last_width = width
            self._last_height = height
            self._needs_upload = True
            print(f"VGEO: Renderer initialized {width}x{height}")

        # Resize if needed
        if width != self._last_width or height != self._last_height:
            self.renderer.resize(width, height)
            self._last_width = width
            self._last_height = height
            self._texture = None  # Recreate texture

        # Upload assets if needed
        if self._needs_upload:
            self._upload_assets()
            self._needs_upload = False

        # Update camera from Blender's view
        self._update_camera(context)

        # Get model transform (identity for now - transforms are in scene)
        identity = np.eye(4, dtype=np.float32).flatten()

        # Render the frame
        self.renderer.render(self.camera, identity)

        # Get pixels and display
        pixels = self.renderer.get_pixels_float()
        self._blit_to_viewport(context, pixels, width, height)

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

            # Get transform as flat 4x4 matrix (column-major for Vulkan)
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
                    self._needs_upload = True
                    print(f"VGEO: Added object '{obj.name}' (ID: {vgeo_id})")

        # Remove objects that no longer exist
        for obj_name in list(self._object_map.keys()):
            if obj_name not in seen_objects:
                vgeo_id = self._object_map.pop(obj_name)
                self.scene.remove_object(vgeo_id)
                self._needs_upload = True
                print(f"VGEO: Removed object '{obj_name}'")

    def _upload_assets(self):
        """Upload all scene assets to GPU"""
        if not self.renderer or not self.renderer.is_initialized:
            return

        for obj_name, vgeo_id in self._object_map.items():
            self.renderer.upload_asset(self.scene, vgeo_id)
            print(f"VGEO: Uploaded asset for '{obj_name}'")

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

        # Get FOV from view - perspective vs ortho
        if rv3d.is_perspective:
            # Perspective view
            self.camera.fov = 50.0  # Default Blender viewport FOV
        else:
            # Ortho view - use a small FOV approximation
            self.camera.fov = 5.0

        self.camera.update()

    def _blit_to_viewport(self, context, pixels, width, height):
        """Blit rendered pixels to viewport using GPU texture"""
        try:
            # Create or resize the intermediate Blender Image
            image_name = "_vgeo_viewport_buffer"
            if self._image is None or self._image.size[0] != width or self._image.size[1] != height:
                # Remove old image if it exists
                if image_name in bpy.data.images:
                    bpy.data.images.remove(bpy.data.images[image_name])

                # Create new image
                self._image = bpy.data.images.new(
                    image_name,
                    width=width,
                    height=height,
                    alpha=True,
                    float_buffer=True
                )
                self._image.colorspace_settings.name = 'Linear'
                self._texture = None  # Force texture recreation

            # Upload pixels to Blender Image
            # pixels is already a flat RGBA float array from get_pixels_float()
            self._image.pixels.foreach_set(pixels)

            # Get GPU texture from image
            if self._texture is None:
                self._texture = gpu.texture.from_image(self._image)

            # Draw the texture covering the entire viewport
            gpu.state.blend_set('NONE')
            draw_texture_2d(self._texture, (0, 0), width, height)

            # Draw indicator that VGEO is active
            self._draw_indicator(context, active=True)

        except Exception as e:
            print(f"VGEO: Blit error: {e}")
            import traceback
            traceback.print_exc()
            self._draw_placeholder(context)

    def _draw_indicator(self, context, active=True):
        """Draw a small indicator showing VGEO status"""
        from gpu_extras.batch import batch_for_shader

        gpu.state.blend_set('ALPHA')
        shader = gpu.shader.from_builtin('UNIFORM_COLOR')

        # Green indicator for active, red for error
        color = (0.2, 0.8, 0.3, 0.7) if active else (0.8, 0.2, 0.2, 0.7)

        vertices = [
            (10, 10), (80, 10), (80, 25), (10, 25)
        ]
        shader.bind()
        shader.uniform_float("color", color)

        batch = batch_for_shader(shader, 'TRI_FAN', {"pos": vertices})
        batch.draw(shader)

        gpu.state.blend_set('NONE')

    def _draw_placeholder(self, context):
        """Draw a placeholder when rendering isn't available"""
        from gpu_extras.batch import batch_for_shader

        region = context.region
        gpu.state.blend_set('ALPHA')
        shader = gpu.shader.from_builtin('UNIFORM_COLOR')

        # Gray background
        vertices = [
            (0, 0), (region.width, 0),
            (region.width, region.height), (0, region.height)
        ]
        shader.bind()
        shader.uniform_float("color", (0.1, 0.1, 0.15, 1.0))

        batch = batch_for_shader(shader, 'TRI_FAN', {"pos": vertices})
        batch.draw(shader)

        self._draw_indicator(context, active=False)
        gpu.state.blend_set('NONE')

    def _draw_fallback(self, context):
        """Draw fallback when native module not available"""
        self._draw_placeholder(context)

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
        # For now, render using our Vulkan renderer
        if NATIVE_AVAILABLE and self.renderer:
            self._render_vgeo()
        else:
            self._render_test_pattern()

    def _render_vgeo(self):
        """Render using VGEO renderer for final output"""
        # Initialize renderer at render resolution
        if not self.renderer.is_initialized:
            self.renderer.init(self.size_x, self.size_y)
        else:
            self.renderer.resize(self.size_x, self.size_y)

        # Upload assets
        self._upload_assets()

        # Set up camera for render
        # TODO: Use render camera settings
        self.camera.aspect = self.size_x / max(self.size_y, 1)
        self.camera.update()

        # Render
        identity = np.eye(4, dtype=np.float32).flatten()
        self.renderer.render(self.camera, identity)

        # Get pixels
        pixels = self.renderer.get_pixels_float()

        # Update render result
        result = self.begin_result(0, 0, self.size_x, self.size_y)
        layer = result.layers[0].passes["Combined"]
        layer.rect = pixels.tolist()
        self.end_result(result)

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
