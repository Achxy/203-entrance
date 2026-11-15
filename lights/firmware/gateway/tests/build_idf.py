#!/usr/bin/env python3
"""Compile the isolated gateway against the pinned SDK; never flash hardware."""
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools"))
import entrance  # noqa: E402

ROOT = Path(__file__).resolve().parent
BUILD = REPO / ".local/lights-gateway-idf"
environment = entrance.idf_env()
environment["IDF_COMPONENT_MANAGER"] = "0"
subprocess.run([
    str(entrance.runtime()), str(entrance.SDK / "tools/idf.py"),
    "-C", str(ROOT / "idf"), "-B", str(BUILD),
    "-D", f"SDKCONFIG={BUILD / 'sdkconfig'}",
    "-D", f"SDKCONFIG_DEFAULTS={ROOT / 'idf/sdkconfig.defaults'}",
    "build",
], env=environment, check=True)
