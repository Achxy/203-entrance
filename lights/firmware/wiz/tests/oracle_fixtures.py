#!/usr/bin/env python3
"""Replay public pinned-upstream configuration fixtures without importing it."""
import ast
import argparse
import json
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[4]
arguments = argparse.ArgumentParser(description=__doc__)
arguments.add_argument("--upstream", type=Path, default=REPO / ".local/lights-research/pywizlight")
arguments.add_argument("--runner", type=Path, default=Path(__file__).resolve().parents[1] / ".build/wiz_fixture_checks")
options = arguments.parse_args()
UPSTREAM = options.upstream.resolve()
PIN = "19e607afe0eb6db1473602639e194d691234d4fd"
if subprocess.check_output(["git", "-C", str(UPSTREAM), "rev-parse", "HEAD"], text=True).strip() != PIN:
    raise SystemExit("Refusing an unpinned upstream fixture checkout")
tree = ast.parse((UPSTREAM / "pywizlight/tests/fake_bulb.py").read_text())
methods = {"getSystemConfig": 1, "getModelConfig": 2, "getUserConfig": 3}
records = []
nonliteral = 0
for node in tree.body:
    name = None
    if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
        name = node.targets[0].id
    elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
        name = node.target.id
    if name not in {"MODULE_CONFIGS", "SYSTEM_CONFIGS", "USER_CONFIGS"}:
        continue
    if not isinstance(node.value, ast.Dict):
        raise SystemExit("Upstream fixture dictionary changed")
    for label_node, payload_node in zip(node.value.keys, node.value.values):
        label = ast.literal_eval(label_node)
        try:
            payload = ast.literal_eval(payload_node)
        except ValueError:
            # Upstream also stores exception classes to simulate failures;
            # they are not wire replies and must never be executed/imported.
            nonliteral += 1
            continue
        if payload["method"] not in methods:
            raise SystemExit(f"Unexpected fixture method: {label!r}")
        mac = payload.get("result", {}).get("mac", "a8bb5006033d")
        records.append(f"{methods[payload['method']]}\t{mac}\t" + json.dumps(payload, separators=(",", ":")))
if len(records) < 30:
    raise SystemExit("Incomplete fixture corpus")
runner = options.runner.resolve()
print(f"Replaying {len(records)} literal wire fixtures; {nonliteral} exception fixtures excluded", flush=True)
result = subprocess.run([str(runner)], input="\n".join(records) + "\n", text=True)
sys.exit(result.returncode)
