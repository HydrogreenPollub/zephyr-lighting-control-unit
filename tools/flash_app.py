#!/usr/bin/env python3
"""Build and flash the application via JLink (sector erase only — preserves MCUboot)."""

import subprocess
import sys
from pathlib import Path

project_dir = Path(__file__).resolve().parent.parent
workspace_dir = project_dir.parent
build_dir = project_dir / "build"

print("=" * 58)
print(" Building app for hydrogreen/lighting_control_unit")
print("=" * 58)

ret = subprocess.run([
    "west", "build", "-b", "lighting_control_unit",
    str(project_dir),
    "-d", str(build_dir),
], cwd=str(workspace_dir))

if ret.returncode != 0:
    print("\nApp build FAILED.", file=sys.stderr)
    sys.exit(ret.returncode)

print()
print("=" * 58)
print(" Flashing app via JLink")
print("=" * 58)

ret = subprocess.run([
    "west", "flash", "--runner", "jlink",
    "--build-dir", str(build_dir),
], cwd=str(workspace_dir))

if ret.returncode != 0:
    print("\nApp flash FAILED.", file=sys.stderr)
    sys.exit(ret.returncode)

print("\nApp flashed successfully.")
