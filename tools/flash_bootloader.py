#!/usr/bin/env python3
"""Build and flash MCUboot via JLink (full-chip erase)."""

import subprocess
import sys
from pathlib import Path

tools_dir = Path(__file__).resolve().parent
project_dir = tools_dir.parent
workspace_dir = project_dir.parent
build_dir = project_dir / "build_mcuboot"

ret = subprocess.run([sys.executable, str(tools_dir / "build_bootloader.py")])
if ret.returncode != 0:
    sys.exit(ret.returncode)

print("\nFlashing MCUboot via JLink")
ret = subprocess.run([
    "west", "flash", "--runner", "jlink", "--erase",
    "--build-dir", str(build_dir),
], cwd=str(workspace_dir))

if ret.returncode != 0:
    print("\nMCUboot flash FAILED.", file=sys.stderr)
    sys.exit(ret.returncode)

print("\nMCUboot flashed successfully.")
