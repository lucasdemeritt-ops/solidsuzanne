"""Zip addons/vgeo (with its built native library) into an installable add-on.

    python tools/package_addon.py [out.zip]

Install the zip in Blender: Preferences > Add-ons > Install from Disk.
"""

import os
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "addons", "vgeo")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build-stream", "vgeo_addon.zip")
    libs = [f for f in os.listdir(os.path.join(SRC, "bin"))] if os.path.isdir(os.path.join(SRC, "bin")) else []
    if not any(f.startswith("vgeo_stream") for f in libs):
        sys.exit("addons/vgeo/bin has no vgeo_stream library: build it first (see docs/NATIVE_STREAMING.md)")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(SRC):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            for f in files:
                if f.endswith((".pyc", ".pdb", ".ilk", ".exp", ".lib")):
                    continue
                full = os.path.join(root, f)
                z.write(full, os.path.join("vgeo", os.path.relpath(full, SRC)))
        # the web viewer ships inside the add-on for Export for Web
        for f in ("vgeo-viewer.js", "meshopt_decoder.mjs"):
            z.write(os.path.join(REPO, "web", f), os.path.join("vgeo", "web", f))
    print("wrote", out)


if __name__ == "__main__":
    main()
