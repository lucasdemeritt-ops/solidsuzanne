"""Export a virtualized object as a self-contained web folder.

    <dir>/<name>.vgeow          compact asset (meshopt-compressed, quantized)
    <dir>/vgeo-viewer.js        WebGPU viewer (one ES module)
    <dir>/meshopt_decoder.mjs   decoder for the compressed streams (MIT, meshoptimizer)
    <dir>/index.html            page that opens the asset

Only files this exporter writes are touched; an existing index.html that it
did not write is left alone (the page is then written as <name>.html).
"""

import json
import os
import shutil

import bpy

from . import native, stream

VIEWER_FILES = ("vgeo-viewer.js", "meshopt_decoder.mjs")
MARKER = "<!-- written by the VGEO add-on -->"


def viewer_dir():
    """Viewer sources: bundled in the add-on zip, or the repository's web/ folder when developing."""
    here = os.path.dirname(__file__)
    for d in (os.path.join(here, "web"), os.path.join(here, "..", "..", "web")):
        if all(os.path.exists(os.path.join(d, f)) for f in VIEWER_FILES):
            return os.path.abspath(d)
    raise RuntimeError("viewer files not found (expected vgeo-viewer.js and meshopt_decoder.mjs)")


PAGE = """<!doctype html>
{marker}
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
  html, body {{ margin: 0; height: 100%; background: {bg}; font: 14px system-ui, sans-serif; color: #dde3ea; }}
  canvas {{ position: fixed; inset: 0; width: 100%; height: 100%; display: block; }}
  #msg {{ position: fixed; inset: 0; display: grid; place-items: center; padding: 16px; text-align: center; }}
  [hidden] {{ display: none !important; }}
</style>
</head>
<body>
<canvas id="view"></canvas>
<div id="msg">Loading…</div>
<script type="module">
import {{ createViewer }} from "./vgeo-viewer.js";
const msg = document.getElementById("msg");
try {{
  await createViewer(document.getElementById("view"), {asset}, {{
    ...{options},
    onProgress: (f) => {{ msg.textContent = `Loading… ${{Math.round(f * 100)}}%`; }},
  }});
  msg.hidden = true;
}} catch (e) {{
  msg.textContent = "This 3D view needs a browser with WebGPU (" + (e.message || e) + ")";
}}
</script>
</body>
</html>
"""


def build_web_asset(obj, path, depsgraph=None, max_triangles=128):
    """Write a .vgeow for any mesh object; returns {"bytes", "source_triangles", "aabb_min", "aabb_max"}.

    Public entry point for other add-ons (WebBlend's Streamed 3D target).
    A virtualized proxy exports from its existing .vgeo; a plain mesh is
    virtualized into a temporary .vgeo first (modifiers applied).
    """
    import tempfile
    from . import build
    if obj.type != 'MESH':
        raise RuntimeError(f"'{obj.name}' is not a mesh")
    if obj.vgeo.uid and obj.vgeo.path:
        asset = native.Asset(stream.asset_path(obj))
        tmpdir = None
    else:
        tmpdir = tempfile.mkdtemp(prefix="vgeo_web_")
        src = os.path.join(tmpdir, "asset.vgeo")
        arrays = build.mesh_arrays(obj, depsgraph or bpy.context.evaluated_depsgraph_get())
        mats = arrays["material_list"]
        native.build(src, arrays["positions"], arrays["normals"], arrays["uvs"], arrays["materials"],
                     [m.name if m else "" for m in mats], max_triangles=max_triangles,
                     indices=arrays["indices"], material_params=build.material_params(mats))
        asset = native.Asset(src)
    try:
        size = asset.export_web(path)
        info = asset.info
        return {"bytes": size, "source_triangles": info["source_triangles"],
                "aabb_min": list(info["aabb_min"]), "aabb_max": list(info["aabb_max"])}
    finally:
        asset.close()
        if tmpdir:
            shutil.rmtree(tmpdir, ignore_errors=True)


FALLBACK_OBJECT = "vgeo_fallback_temp"


def build_fallback_object(obj, max_triangles=150_000, depsgraph=None):
    """A temporary mesh object with the finest uniform level under `max_triangles`, placed like
    `obj` and with its materials: a light stand-in for glTF export (e.g. no-WebGPU fallbacks).
    Remove it with remove_fallback_object."""
    import tempfile
    from . import build
    tmpdir = None
    if obj.vgeo.uid and obj.vgeo.path:
        asset = native.Asset(stream.asset_path(obj))
        materials = tuple(obj.data.materials)
    else:
        tmpdir = tempfile.mkdtemp(prefix="vgeo_fb_")
        src = os.path.join(tmpdir, "asset.vgeo")
        arrays = build.mesh_arrays(obj, depsgraph or bpy.context.evaluated_depsgraph_get())
        mats = arrays["material_list"]
        native.build(src, arrays["positions"], arrays["normals"], arrays["uvs"], arrays["materials"],
                     [m.name if m else "" for m in mats], indices=arrays["indices"],
                     material_params=build.material_params(mats))
        asset = native.Asset(src)
        materials = tuple(mats)
    try:
        chosen = None
        for depth in range(asset.info["lod_levels"]):
            asset.select_level(depth)
            if asset.last.triangles <= max_triangles:
                chosen = depth
                break
        data = asset.level_mesh_data(chosen if chosen is not None else -1)
        me = bpy.data.meshes.new(FALLBACK_OBJECT)
        stream._sync_materials(me, materials)
        stream.fill_mesh(me, data, materials, False)
        temp = bpy.data.objects.new(FALLBACK_OBJECT, me)
        bpy.context.scene.collection.objects.link(temp)
        temp.matrix_world = obj.matrix_world.copy()
        return temp
    finally:
        asset.close()
        if tmpdir:
            shutil.rmtree(tmpdir, ignore_errors=True)


def remove_fallback_object(temp):
    me = temp.data
    bpy.data.objects.remove(temp)
    if me is not None and me.users == 0:
        bpy.data.meshes.remove(me)


def export(obj, directory, with_page=True):
    if not directory:
        raise RuntimeError("no output folder")
    rt = stream.runtime_for(obj)
    if rt.asset is None:
        raise RuntimeError(rt.error or "asset not loaded")
    os.makedirs(directory, exist_ok=True)
    name = bpy.path.clean_name(obj.name) or "asset"
    asset = name + ".vgeow"
    size = rt.asset.export_web(os.path.join(directory, asset))
    written = [asset]
    if with_page:
        src = viewer_dir()
        for f in VIEWER_FILES:
            shutil.copyfile(os.path.join(src, f), os.path.join(directory, f))
            written.append(f)
        page = "index.html"
        existing = os.path.join(directory, page)
        if os.path.exists(existing):
            with open(existing, encoding="utf-8", errors="replace") as fh:
                if MARKER not in fh.read(4096):
                    page = name + ".html"  # never overwrite a page we did not write
        options = {"pixelError": float(obj.vgeo.pixel_error)}
        with open(os.path.join(directory, page), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(PAGE.format(marker=MARKER, title=obj.name, bg="#9fb2c8",
                                 asset=json.dumps(asset), options=json.dumps(options)))
        written.append(page)
    return {"asset": asset, "bytes": size, "files": written}
