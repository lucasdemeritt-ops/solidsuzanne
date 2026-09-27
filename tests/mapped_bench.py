"""Open/extract cost and memory of the native runtime, without Blender.

    python tests/mapped_bench.py ASSET.vgeo [--addons DIR] [--handles 4]

Opens the asset several times, picks a close-up cut and a full-detail level,
extracts every chunk, and prints timings plus the process's private memory
(Windows) after each step. --addons points at another checkout's addons/
folder to compare library versions.
"""

import ctypes
import os
import sys
import time

argv = sys.argv[1:]
ASSET = argv[0]
ADDONS = argv[argv.index("--addons") + 1] if "--addons" in argv else os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "addons")
HANDLES = int(argv[argv.index("--handles") + 1]) if "--handles" in argv else 4
sys.path.insert(0, os.path.join(ADDONS, "vgeo"))
import native  # noqa: E402  (plain module import: no bpy needed)


def private_mb():
    if sys.platform != "win32":
        return 0.0

    class PMC(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_uint32), ("PageFaultCount", ctypes.c_uint32),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                    ("PrivateUsage", ctypes.c_size_t)]
    c = PMC()
    c.cb = ctypes.sizeof(PMC)
    k32 = ctypes.WinDLL("kernel32")
    psapi = ctypes.WinDLL("psapi")
    k32.GetCurrentProcess.restype = ctypes.c_void_p
    psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(PMC), ctypes.c_uint32]
    psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(c), c.cb)
    return c.PrivateUsage / 2**20


def main():
    size = os.path.getsize(ASSET) / 2**20
    print(f"library v{native.lib().vgeo_version()}  asset {size:.0f} MB  private {private_mb():.0f} MB")
    t = time.perf_counter()
    assets = [native.Asset(ASSET) for _ in range(HANDLES)]
    print(f"open x{HANDLES}: {1000 * (time.perf_counter() - t) / HANDLES:.0f} ms each, private {private_mb():.0f} MB")
    a = assets[0]
    lo, hi = a.info["aabb_min"], a.info["aabb_max"]
    c = [(lo[i] + hi[i]) / 2 for i in range(3)]
    ext = max(hi[i] - lo[i] for i in range(3))
    cam = (c[0], c[1] - ext * 0.3, hi[2] + ext * 0.02)   # low over the surface: a mixed-level cut
    view = native.make_view(cam, 1.0 / 0.414, ext * 1e-5, 1.0 / 1080)
    for label, pick in (("close-up cut", lambda: a.select([view])), ("full detail", lambda: a.select_level(0))):
        pick()
        t = time.perf_counter()
        if hasattr(a, "prefetch"):
            a.prefetch(list(range(a.chunk_count)))
        tris = 0
        for k in range(a.chunk_count):
            d = a.extract(k)
            tris += d["tri_count"] if d else 0
        dt = time.perf_counter() - t
        print(f"{label}: {tris:,} tris extracted in {dt:.2f} s ({tris / dt / 1e6:.1f} M tris/s), "
              f"private {private_mb():.0f} MB")
    if hasattr(a, "memory"):
        print("memory() of handle 0:", [f"{x / 2**20:.0f} MB" for x in a.memory()])
    for x in assets:
        x.close()


main()
