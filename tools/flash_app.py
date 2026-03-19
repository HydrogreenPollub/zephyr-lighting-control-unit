#!/usr/bin/env python3
"""Build and flash the application via JLink (sector erase only — preserves MCUboot)."""

import subprocess
import sys
from pathlib import Path

tools_dir = Path(__file__).resolve().parent
project_dir = tools_dir.parent
workspace_dir = project_dir.parent
build_dir = project_dir / "build"

ret = subprocess.run([sys.executable, str(tools_dir / "build_app.py")])
if ret.returncode != 0:
    sys.exit(ret.returncode)

print("\nFlashing app via JLink")
ret = subprocess.run([
    "west", "flash", "--runner", "jlink",
    "--build-dir", str(build_dir),
], cwd=str(workspace_dir))

if ret.returncode != 0:
    print("\nApp flash FAILED.", file=sys.stderr)
    sys.exit(ret.returncode)

print("\nApp flashed successfully.")
