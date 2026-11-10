import ctypes
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Settings(ctypes.Structure):
    _fields_=[('schema',ctypes.c_uint32),('generation',ctypes.c_uint32),('ssid',ctypes.c_char*33),
              ('password',ctypes.c_char*65),('enabled',ctypes.c_uint8),('reserved',ctypes.c_uint8*5),('crc',ctypes.c_uint32)]
class SettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();out=Path(cls.temp.name)/'settings.so'
        subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror','-I',str(ROOT/'components/entrance/include'),
                        str(ROOT/'components/entrance/settings_policy.c'),str(ROOT/'components/entrance/core.c'),'-o',str(out)],check=True)
        cls.lib=ctypes.CDLL(str(out));cls.lib.record_crc.argtypes=[ctypes.c_void_p,ctypes.c_size_t];cls.lib.record_crc.restype=ctypes.c_uint32
        cls.lib.settings_valid.argtypes=[ctypes.POINTER(Settings)];cls.lib.settings_valid.restype=ctypes.c_bool
        cls.lib.settings_select.argtypes=[ctypes.POINTER(Settings),ctypes.c_bool,ctypes.POINTER(Settings),ctypes.c_bool,ctypes.POINTER(Settings)]
        cls.lib.settings_select.restype=ctypes.c_bool
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def snapshot(self,generation,schema=1):
        s=Settings(schema=schema,generation=generation,ssid=b'qualification',password=b'example-password',enabled=1)
        s.crc=self.lib.record_crc(ctypes.byref(s),Settings.crc.offset);return s
    def select(self,a,b,read_a=True,read_b=True):
        out=Settings();ok=self.lib.settings_select(ctypes.byref(a),read_a,ctypes.byref(b),read_b,ctypes.byref(out));return ok,out
    def test_select_latest_complete_snapshot(self):
        for a,b in [(1,2),(20,19),(0,1)]:
            ok,out=self.select(self.snapshot(a),self.snapshot(b));self.assertTrue(ok);self.assertEqual(out.generation,max(a,b))
    def test_torn_new_snapshot_preserves_previous(self):
        previous=self.snapshot(7);candidate=self.snapshot(8)
        raw=bytearray(bytes(candidate))
        for index in range(len(raw)):
            bad=bytearray(raw);bad[index]^=1
            ok,out=self.select(previous,Settings.from_buffer_copy(bad))
            self.assertTrue(ok);self.assertEqual(out.generation,7);self.assertEqual(out.password,previous.password)
    def test_future_schema_cannot_hide_rollback_configuration(self):
        ok,out=self.select(self.snapshot(2),self.snapshot(3,schema=2))
        self.assertTrue(ok);self.assertEqual(out.schema,1);self.assertEqual(out.generation,2)
    def test_storage_failure_and_incompatible_snapshots(self):
        a,b=self.snapshot(2),self.snapshot(3)
        self.assertFalse(self.select(a,b,False,False)[0])
        self.assertEqual(self.select(a,b,True,False)[1].generation,2)
        self.assertFalse(self.select(self.snapshot(1,2),self.snapshot(2,3))[0])
