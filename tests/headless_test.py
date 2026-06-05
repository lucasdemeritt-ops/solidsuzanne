"""
VGEO Headless Tests
Exercises vgeo_build.exe and vgeo_validate.exe against test assets.
No GPU, no Blender, no display required.
"""

import subprocess
import os
import sys
import struct
import tempfile
import shutil

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_RELEASE = os.path.join(REPO, "build")
VGEO_BUILD   = os.path.join(BUILD_RELEASE, "tools", "vgeo_build",   "Release", "vgeo_build.exe")
VGEO_VALIDATE= os.path.join(BUILD_RELEASE, "tools", "vgeo_validate", "Release", "vgeo_validate.exe")
ASSETS       = os.path.join(REPO, "tests", "assets")

VGEO_MAGIC = b"VGEO"

PASS = "[PASS]"
FAIL = "[FAIL]"
results = []

def check(name, ok, detail=""):
    tag = PASS if ok else FAIL
    msg = f"  {tag} {name}"
    if detail:
        msg += f"  ({detail})"
    print(msg)
    results.append((name, ok))
    return ok

def run(cmd, timeout=30):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "TIMEOUT"
    except FileNotFoundError as e:
        return -2, "", str(e)

# ── Prerequisites ─────────────────────────────────────────────────────────────

print("\n=== Prerequisites ===")
check("vgeo_build.exe exists",   os.path.exists(VGEO_BUILD),   VGEO_BUILD)
check("vgeo_validate.exe exists", os.path.exists(VGEO_VALIDATE), VGEO_VALIDATE)
check("cube.obj exists",    os.path.exists(os.path.join(ASSETS, "cube.obj")))
check("icosphere.obj exists", os.path.exists(os.path.join(ASSETS, "icosphere.obj")))

# ── Conversion ────────────────────────────────────────────────────────────────

print("\n=== Conversion (vgeo_build) ===")

tmpdir = tempfile.mkdtemp(prefix="vgeo_test_")

def convert(name, obj_file):
    obj_path  = os.path.join(ASSETS, obj_file)
    vgeo_path = os.path.join(tmpdir, name + ".vgeo")
    code, out, err = run([VGEO_BUILD, obj_path, vgeo_path])
    ok_code = check(f"{name}: exit code 0", code == 0,
                    f"code={code} stderr={err.strip()[:120]}")
    ok_file = check(f"{name}: output file created", os.path.exists(vgeo_path))
    return vgeo_path if (ok_code and ok_file) else None

cube_vgeo = convert("cube", "cube.obj")
ico_vgeo  = convert("icosphere", "icosphere.obj")

# ── Magic bytes & size sanity ─────────────────────────────────────────────────

print("\n=== File Integrity ===")

def check_magic(name, path):
    if not path:
        check(f"{name}: magic bytes", False, "file missing")
        return
    with open(path, "rb") as f:
        magic = f.read(4)
        size  = os.path.getsize(path)
    check(f"{name}: VGEO magic bytes", magic == VGEO_MAGIC,
          repr(magic) if magic != VGEO_MAGIC else f"{size} bytes")
    check(f"{name}: file size > 0", size > 0, f"{size} bytes")

check_magic("cube",      cube_vgeo)
check_magic("icosphere", ico_vgeo)

# ── Validate tool ─────────────────────────────────────────────────────────────

print("\n=== Validation (vgeo_validate) ===")

def validate(name, path):
    if not path:
        check(f"{name}: validate", False, "file missing")
        return
    code, out, err = run([VGEO_VALIDATE, path])
    combined = (out + err).strip()
    check(f"{name}: vgeo_validate exit 0", code == 0,
          combined[:120] if code != 0 else "")
    # Print any stats the validator outputs
    for line in combined.splitlines():
        if line.strip():
            print(f"    > {line}")

validate("cube",      cube_vgeo)
validate("icosphere", ico_vgeo)

# ── Meshlet count sanity ──────────────────────────────────────────────────────

print("\n=== Meshlet Sanity (vgeo_validate output) ===")
# cube has 12 tris — should fit in a single meshlet (max ~126 tris)
# icosphere has 80 tris — should also be a single meshlet
# Just a smoke check that conversion didn't silently produce empty output

def file_size_check(name, path, min_bytes=100):
    if not path:
        return
    size = os.path.getsize(path) if path and os.path.exists(path) else 0
    check(f"{name}: file looks non-trivial (>{min_bytes}B)", size > min_bytes, f"{size} bytes")

file_size_check("cube",      cube_vgeo)
file_size_check("icosphere", ico_vgeo)

# ── Round-trip: re-convert the same OBJ twice, compare sizes ─────────────────

print("\n=== Determinism (double-convert cube) ===")
cube2 = os.path.join(tmpdir, "cube2.vgeo")
run([VGEO_BUILD, os.path.join(ASSETS, "cube.obj"), cube2])
if cube_vgeo and os.path.exists(cube2):
    s1 = os.path.getsize(cube_vgeo)
    s2 = os.path.getsize(cube2)
    check("cube: two conversions produce same file size", s1 == s2, f"{s1} vs {s2}")

# ── vgeo_build: bad input handling ───────────────────────────────────────────

print("\n=== Error Handling ===")
code, _, _ = run([VGEO_BUILD, os.path.join(tmpdir, "nonexistent.obj"),
                               os.path.join(tmpdir, "should_not_exist.vgeo")])
check("bad input: non-zero exit code", code != 0, f"code={code}")
check("bad input: no output file created",
      not os.path.exists(os.path.join(tmpdir, "should_not_exist.vgeo")))

# ── Cleanup ───────────────────────────────────────────────────────────────────

shutil.rmtree(tmpdir, ignore_errors=True)

# ── Summary ───────────────────────────────────────────────────────────────────

print("\n=== Summary ===")
passed = sum(1 for _, ok in results if ok)
failed = sum(1 for _, ok in results if not ok)
print(f"  {passed} passed, {failed} failed\n")

if failed:
    print("Failed tests:")
    for name, ok in results:
        if not ok:
            print(f"  {FAIL} {name}")

sys.exit(0 if failed == 0 else 1)
