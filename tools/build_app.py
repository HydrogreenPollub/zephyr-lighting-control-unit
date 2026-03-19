#!/usr/bin/env python3
"""Build the application."""

import subprocess
import sys
from pathlib import Path

project_dir = Path(__file__).resolve().parent.parent
workspace_dir = project_dir.parent
build_dir = project_dir / "build"

board = next((project_dir / "boards" / "hydrogreen").iterdir()).name

print(f"Building app for hydrogreen/{board}")
ret = subprocess.run([
    "west", "build", "-b", board,
    str(project_dir),
    "-d", str(build_dir),
], cwd=str(workspace_dir))

sys.exit(ret.returncode)
