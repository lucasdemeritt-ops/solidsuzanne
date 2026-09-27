import sys, json
sys.path.insert(0, r"C:\Users\Shadow\blender\solidsuzanne\addons")
from vgeo import native
import os
S = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else os.getcwd()  # folder with web_rock.json / web_terrain.json
for asset, res in (("rock.vgeo", "web_rock.json"), ("terrain2m.vgeo", "web_terrain.json")):
    a = native.Asset(r"C:\Users\Shadow\blender\solidsuzanne\web\assets" + "\\" + asset)
    for v in json.load(open(S + "\\" + res)):
        a.select([native.make_view(v["eye"], v["proj"], v["near"], v["threshold"])])
        n = int(a.last.triangles)
        print(f"CMP {asset:15s} d={v['distance']:<6} px={v['px']:<4} web={v['triangles']:>9,} native={n:>9,} "
              f"clusters web={v['clusters']} native={a.last.clusters} {'MATCH' if n == v['triangles'] else 'DIFF'}")
