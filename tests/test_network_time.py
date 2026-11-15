"""Exercise the lighting clock policy without changing any host/device clock."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NetworkTimeTests(unittest.TestCase):
    def test_received_time_and_monotonic_freshness(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "network-time"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wconversion", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "components/entrance/include"),
                "-I", str(ROOT / "lights/firmware/runtime"),
                str(ROOT / "tests/native/network_time_test.c"), "-o", str(output),
            ], check=True)
            subprocess.run([str(output)], check=True, timeout=10)
