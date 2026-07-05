"""
VGEO Headless Tests
Exercises vgeo_build and vgeo_validate against test assets.
No GPU, no Blender, no display required.

The build directory can be overridden with the VGEO_BUILD_DIR env var.
"""

import subprocess
import os
import sys
import filecmp
import tempfile
import shutil

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.environ.get("VGEO_BUILD_DIR", os.path.join(REPO, "build"))
ASSETS = os.path.join(REPO, "tests", "assets")

EXE = ".exe" if os.name == "nt" else ""


def find_tool(name):
    """Locate a tool binary across single-config (Linux/Mac) and
    multi-config (MSVC Release/Debug) build layouts."""
    candidates = [
        os.path.join(BUILD_DIR, "tools", name, name + EXE),
        os.path.join(BUILD_DIR, "tools", name, "Release", name + EXE),
        os.path.join(BUILD_DIR, "tools", name, "Debug", name + EXE),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return candidates[0]  # Report the default path in the failure message


VGEO_BUILD = find_tool("vgeo_build")
VGEO_VALIDATE = find_tool("vgeo_validate")

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
prereqs_ok = True
prereqs_ok &= check("vgeo_build exists",    os.path.exists(VGEO_BUILD),    VGEO_BUILD)
prereqs_ok &= check("vgeo_validate exists", os.path.exists(VGEO_VALIDATE), VGEO_VALIDATE)
prereqs_ok &= check("cube.obj exists",      os.path.exists(os.path.join(ASSETS, "cube.obj")))
prereqs_ok &= check("icosphere.obj exists", os.path.exists(os.path.join(ASSETS, "icosphere.obj")))

if not prereqs_ok:
    print("\nPrerequisites missing - build the project first (see README).")
    sys.exit(1)

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
if cube_vgeo:
    if os.path.exists(cube2):
        check("cube: two conversions produce identical bytes",
              filecmp.cmp(cube_vgeo, cube2, shallow=False))
    else:
        check("cube: second conversion produced a file", False, cube2)

# ── vgeo_build: bad input handling ───────────────────────────────────────────

print("\n=== Error Handling ===")
code, _, _ = run([VGEO_BUILD, os.path.join(tmpdir, "nonexistent.obj"),
                               os.path.join(tmpdir, "should_not_exist.vgeo")])
# code -1/-2 mean the tool never ran (timeout / missing binary), which must
# not be mistaken for the tool correctly rejecting bad input
check("bad input: tool ran and rejected it", code > 0, f"code={code}")
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
