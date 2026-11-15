#!/usr/bin/env python3
"""Check local reference hashes and copied PUBLIC fixtures; no network calls."""
from pathlib import Path
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REPO = Path(__file__).resolve().parents[5]
manifest = json.loads((ROOT / "UPSTREAM.json").read_text())
actual = subprocess.check_output(["git", "-C", str(REPO / ".tools/esp-idf"), "rev-parse", "HEAD"], text=True).strip()
if actual != manifest["esp_idf_revision"]:
    raise SystemExit("ESP-IDF checkout differs from the reviewed pin")
for reference in manifest["references"]:
    digest = hashlib.sha256((REPO / reference["path"]).read_bytes()).hexdigest()
    if digest != reference["sha256"]:
        raise SystemExit(f"Reference changed; review before updating browser pins: {reference['path']}")
original = REPO / "lights/security/Tests/LightsSecurityTests/Fixtures/idf61.json"
if (ROOT / "tests/interop-fixtures.json").read_bytes() != original.read_bytes():
    raise SystemExit("Browser public fixture corpus differs from reviewed source")
print(f"Browser protocol source pins: {len(manifest['references'])} hashes and public fixture copy verified")
