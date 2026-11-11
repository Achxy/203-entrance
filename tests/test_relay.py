import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
class RelayTests(unittest.TestCase):
    def test_fragmentation_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'relay-test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'components/entrance/include'),
                            str(ROOT / 'tests/native/relay_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)
