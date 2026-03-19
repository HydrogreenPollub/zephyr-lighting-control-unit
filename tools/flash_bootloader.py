#!/usr/bin/env python3
"""Build and flash MCUboot via JLink (full-chip erase)."""

import subprocess
import sys
from pathlib import Path

project_dir = Path(__file__).resolve().parent.parent
workspace_dir = project_dir.parent
build_dir = project_dir / "build_mcuboot"

print("=" * 58)
print(" Building MCUboot for hydrogreen/lighting_control_unit")
print("=" * 58)

ret = subprocess.run([
    "west", "build", "-b", "lighting_control_unit",
    str(workspace_dir / "bootloader" / "mcuboot" / "boot" / "zephyr"),
    "-d", str(build_dir),
    "--",
    f"-DBOARD_ROOT={project_dir}/",
    f"-DEXTRA_CONF_FILE={project_dir}/mcuboot.conf",
], cwd=str(workspace_dir))

if ret.returncode != 0:
    print("\nMCUboot build FAILED.", file=sys.stderr)
    sys.exit(ret.returncode)

print()
print("=" * 58)
print(" Flashing MCUboot via JLink")
print("=" * 58)

ret = subprocess.run([
    "west", "flash", "--runner", "jlink", "--erase",
    "--build-dir", str(build_dir),
], cwd=str(workspace_dir))

if ret.returncode != 0:
    print("\nMCUboot flash FAILED.", file=sys.stderr)
    sys.exit(ret.returncode)

print("\nMCUboot flashed successfully.")
