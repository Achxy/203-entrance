#!/usr/bin/env python3
"""Compile the isolated WiZ component; never flash or start networking."""
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools"))
import entrance  # noqa: E402 -- reuse the repository's pinned local SDK environment

ROOT = Path(__file__).resolve().parents[1]
environment = entrance.idf_env()
# All dependencies already exist at pinned local paths. Avoid registry access
# and the component manager's unnecessary host process-list inspection.
environment["IDF_COMPONENT_MANAGER"] = "0"
subprocess.run([
    str(entrance.runtime()), str(entrance.SDK / "tools/idf.py"),
    "-C", str(ROOT / "tests/idf"), "-B", str(ROOT / ".idf-build"),
    "-D", f"SDKCONFIG={ROOT / '.idf-build/sdkconfig'}",
    "-D", f"SDKCONFIG_DEFAULTS={ROOT / 'tests/idf/sdkconfig.defaults'}",
    "build",
], env=environment, check=True)
