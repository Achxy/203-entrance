import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class NativeOtaTests(unittest.TestCase):
    def test_production_state_machine(self):
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        if not cjson.exists():
            self.skipTest('Resolve pinned cJSON with the firmware build first')
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'ota-tests'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-I', str(ROOT / 'tests/native/include'),
                            '-I', str(ROOT / 'components/entrance/include'), '-I', str(cjson),
                            str(ROOT / 'tests/native/ota_harness.c'),
                            str(ROOT / 'components/entrance/core.c'), str(cjson / 'cJSON.c'),
                            '-lm', '-o', str(binary)], check=True)
            subprocess.run([binary], check=True, timeout=20)
