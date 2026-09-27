"""Write paged (.vgeow v2) test assets and scenes next to the v1 ones in web/assets.

    python tests/web/make_paged_assets.py

Needs web/assets/rock.vgeo, terrain2m.vgeo and scene_rocks.bin (see web/README.md).
"""
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "addons", "vgeo"))
import native  # noqa: E402

ASSETS = os.path.join(REPO, "web", "assets")
for name in ("rock", "terrain2m"):
    a = native.Asset(os.path.join(ASSETS, name + ".vgeo"))
    v1 = a.export_web(os.path.join(ASSETS, name + ".vgeow"))
    v2 = a.export_web(os.path.join(ASSETS, name + "_paged.vgeow"), paged=True)
    a.close()
    print(f"{name}: v1 {v1 / 2**20:.1f} MB, paged {v2 / 2**20:.1f} MB")
for suffix, src in (("", "rock.vgeow"), ("_paged", "rock_paged.vgeow")):
    with open(os.path.join(ASSETS, f"scene_instances{suffix}.json"), "w") as f:
        json.dump({"objects": [{"src": src, "instances": "scene_rocks.bin"}]}, f)
