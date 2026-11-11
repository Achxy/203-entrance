"""Execute the shipped loop decoder against independently hashed artwork frames."""
import ctypes as C
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class BuiltinLoopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();output=Path(cls.temp.name)/'loop.so'
        sources=['tests/native/loop_harness.c']+['components/entrance/'+f+'.c' for f in ['builtin_loop','media','core','captions']]
        subprocess.run(['cc','-std=c11','-O2','-shared','-fPIC','-Wall','-Wextra','-Werror','-I',str(ROOT/'components/entrance/include'),*[str(ROOT/s) for s in sources],'-o',str(output)],check=True)
        cls.lib=C.CDLL(str(output));cls.lib.open_loop.argtypes=[C.c_void_p,C.c_size_t];cls.lib.seek_loop.argtypes=[C.c_uint];cls.lib.frame_pixels.argtypes=[C.c_void_p]
        cls.asset=(ROOT/'assets/neko/neko.ecl').read_bytes();cls.evidence=json.loads((ROOT/'assets/neko/provenance.json').read_text())
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def open(self,data):
        self.buffer=C.create_string_buffer(data);return self.lib.open_loop(self.buffer,len(data))
    def pixels(self):
        out=C.create_string_buffer(20480*2);n=self.lib.frame_pixels(out);return out.raw[:n]
    def test_every_frame_exact_and_one_hundred_seamless_loops(self):
        self.assertEqual(hashlib.sha256(self.asset).hexdigest(),self.evidence['sha256']);self.assertTrue(self.open(self.asset))
        for loop in range(100):
            for frame,expected in enumerate(self.evidence['frame_sha256_rgb565le']):
                self.assertEqual(self.lib.current_loop_frame(),frame);self.assertEqual(hashlib.sha256(self.pixels()).hexdigest(),expected);self.assertTrue(self.lib.advance_loop())
            self.assertEqual(self.lib.loop_count(),loop+1)
    def test_seek_and_invalid_asset_cannot_keep_stale_playback(self):
        self.assertTrue(self.open(self.asset))
        for f in [23,0,12,3]:
            self.assertTrue(self.lib.seek_loop(f));self.assertEqual(hashlib.sha256(self.pixels()).hexdigest(),self.evidence['frame_sha256_rgb565le'][f])
        self.assertFalse(self.lib.seek_loop(24))
        for n in [0,4,79,80,len(self.asset)-1]:
            self.assertFalse(self.open(self.asset[:n]));self.assertFalse(self.lib.advance_loop())
        broken=bytearray(self.asset);broken[-1]^=1;self.assertFalse(self.open(bytes(broken)))
