#!/usr/bin/env python3
"""Build MCUboot bootloader."""

import subprocess
import sys
from pathlib import Path

project_dir = Path(__file__).resolve().parent.parent
workspace_dir = project_dir.parent
build_dir = project_dir / "build_mcuboot"

board = next((project_dir / "boards" / "hydrogreen").iterdir()).name

print(f"Building MCUboot for hydrogreen/{board}")
ret = subprocess.run([
    "west", "build", "-b", board,
    str(workspace_dir / "bootloader" / "mcuboot" / "boot" / "zephyr"),
    "-d", str(build_dir),
    "--",
    f"-DBOARD_ROOT={project_dir}/",
    f"-DEXTRA_CONF_FILE={project_dir}/mcuboot.conf",
], cwd=str(workspace_dir))

sys.exit(ret.returncode)
