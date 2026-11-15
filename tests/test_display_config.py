"""Keep the previous firmware's display record readable while adding slots."""
import ctypes as C
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]


class DisplayConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        path = Path(cls.temp.name) / 'display-config.so'
        subprocess.run(['cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT/'components/entrance/include'),
                        str(ROOT/'components/entrance/display_config.c'),
                        str(ROOT/'components/entrance/core.c'), '-o', str(path)], check=True)
        cls.lib = C.CDLL(str(path))
        cls.lib.display_config_legacy.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p]
        cls.lib.display_config_valid.argtypes = [C.c_void_p]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_every_legacy_panel_mode_keeps_its_settings_and_selects_neko(self):
        for mode in range(6):
            for rotation in range(4):
                for profile in range(3):
                    for inverted in range(2):
                        prefix = struct.pack('<IBBBB', 0x31444345, mode, rotation, profile, inverted)
                        old = prefix + struct.pack('<I', zlib.crc32(prefix))
                        result = C.create_string_buffer(16)
                        self.assertTrue(self.lib.display_config_legacy(old, len(old), result))
                        self.assertTrue(self.lib.display_config_valid(result))
                        self.assertEqual(result.raw[4:8], old[4:8])
                        self.assertEqual(result.raw[8:12], b'\x01\0\0\0')
                        self.assertEqual(old, prefix+struct.pack('<I', zlib.crc32(prefix)))

    def test_invalid_records_and_slots_are_rejected(self):
        def record(slot=1, reserved=0, mode=4):
            data = struct.pack('<IBBBBBBBB', 0x32444345, mode, 1, 2, 0, slot, reserved, 0, 0)
            return data+struct.pack('<I', zlib.crc32(data))
        for slot in (1, 2):
            self.assertTrue(self.lib.display_config_valid(record(slot)))
        for bad in (record(0), record(3), record(255), record(reserved=1), record(mode=6), record()[:-1]+b'\0'):
            self.assertFalse(self.lib.display_config_valid(bad))
        out = C.create_string_buffer(16)
        self.assertFalse(self.lib.display_config_legacy(record(), 16, out))
        self.assertFalse(self.lib.display_config_legacy(b'\0'*12, 12, out))
