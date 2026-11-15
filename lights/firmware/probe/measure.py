#!/usr/bin/env python3
"""Build isolated size stages; never flash or interact with hardware."""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

PROJECT = Path(__file__).resolve().parent
ROOT = PROJECT.parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from entrance import idf_env, runtime, SDK  # noqa: E402
from firmware import IDF_COMMIT  # noqa: E402

OUT = ROOT / ".local/lights-resource-probe"
BUILD = OUT / "build"

def execute(args, output, env):
    with output.open("w") as log:
        result = subprocess.run([str(x) for x in args], cwd=PROJECT, env=env,
                                stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print(output.read_text()[-10000:], flush=True)
        raise SystemExit(result.returncode)

def main():
    actual = subprocess.check_output(["git", "-C", str(SDK), "rev-parse", "HEAD"], text=True).strip()
    if actual != IDF_COMMIT:
        raise SystemExit("Pinned SDK revision mismatch")
    OUT.mkdir(parents=True, exist_ok=True)
    env = idf_env()
    # All dependencies are already pinned and local. Avoid dependency-manager
    # process inspection/downloads during this isolated offline measurement.
    env["IDF_COMPONENT_MANAGER"] = "0"
    summaries = []
    for stage in (0, 1, 2, 3):
        print(f"Building isolated resource stage {stage}", flush=True)
        start = time.monotonic()
        execute([runtime(), SDK / "tools/idf.py", "-C", PROJECT, "-B", BUILD,
                 "-D", f"SDKCONFIG={OUT / 'sdkconfig'}", "-D", "IDF_TARGET=esp32c3",
                 "-D", f"PROBE_STAGE={stage}", "build"], OUT / f"stage-{stage}-build.log", env)
        for suffix in ("map", "elf", "bin"):
            shutil.copy2(BUILD / f"lights-resource-probe.{suffix}", OUT / f"stage-{stage}.{suffix}")
        execute([runtime(), "-m", "esp_idf_size", "--format", "json2",
                 OUT / f"stage-{stage}.map"], OUT / f"stage-{stage}-size.json", env)
        execute([runtime(), "-m", "esp_idf_size", "--archives", "--format", "json2",
                 OUT / f"stage-{stage}.map"], OUT / f"stage-{stage}-archives.json", env)
        data = json.loads((OUT / f"stage-{stage}-size.json").read_text())
        data.update(stage=stage, elapsed_s=round(time.monotonic()-start, 2),
                    binary_bytes=(OUT / f"stage-{stage}.bin").stat().st_size,
                    binary_sha256=hashlib.sha256((OUT / f"stage-{stage}.bin").read_bytes()).hexdigest())
        summaries.append(data)
        print(json.dumps(data), flush=True)
    (OUT / "probe-summary.json").write_text(json.dumps({"idf": actual, "stages": summaries}, indent=2)+"\n")

if __name__ == "__main__":
    main()
