"""Run the shipped C replay decoder against independent packing and raster fixtures."""
import binascii
import ctypes as C
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from replay import pack, inspect, SCENE_BYTES

class ReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        p = Path(cls.temp.name)
        output = p/'replay.so'
        subprocess.run(['cc','-O2','-shared','-fPIC','-Wall','-Wextra','-Werror','-I',str(ROOT/'components/entrance/include'),str(ROOT/'tests/native/replay_harness.c'),str(ROOT/'components/entrance/replay.c'),str(ROOT/'components/entrance/replay_scene.c'),'-lz','-o',str(output)],check=True)
        cls.lib = C.CDLL(str(output)); cls.lib.open_replay.argtypes=[C.c_void_p,C.c_size_t]
        cls.lib.get_scene.argtypes=cls.lib.get_frame.argtypes=[C.c_void_p]
        cls.lib.seek_frame.argtypes=[C.c_uint]
        cls.scenes=[]
        for i in range(280):
            s=bytearray(SCENE_BYTES);s[2048:2304]=bytes([248,0,0,0])*64;s[2304:2336]=bytes(range(32))
            s[2404]=1;s[2346]=0;s[2347]=30
            struct.pack_into('<H',s,2342,i%512)
            for j in range(20):s[j*47]=(i+j)%256
            cls.scenes.append(bytes(s))
        tiles=bytearray(8192);tiles[16:24]=b'\xff'*8
        cls.tiles=bytes(tiles);cls.palette=bytes(128)
        cls.asset=pack(cls.scenes,cls.tiles,cls.palette,[{'frame':0,'world':1,'level':1},{'frame':150,'world':1,'level':2}],100000)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def open(self,b):
        self.buffer=C.create_string_buffer(bytes(b))
        return bool(self.lib.open_replay(self.buffer,len(b)))

    def scene(self):
        out=C.create_string_buffer(SCENE_BYTES);self.lib.get_scene(out);return out.raw

    def test_exact_deltas_block_boundaries_seek_and_loop(self):
        self.assertTrue(self.open(self.asset))
        for i,expected in enumerate(self.scenes):
            self.assertEqual(self.lib.current_frame(),i);self.assertEqual(self.scene(),expected)
            self.assertTrue(self.lib.advance())
        self.assertEqual(self.lib.current_frame(),0);self.assertEqual(self.lib.loop_count(),1)
        for i in (279,0,127,150,128,1):
            self.assertTrue(self.lib.seek_frame(i));self.assertEqual(self.scene(),self.scenes[i])
        self.assertFalse(self.lib.seek_frame(len(self.scenes)))
        self.assertEqual(inspect(self.asset)['frames'],280)

    def test_pixel_planes_scroll_sprite_flip_priority_and_clipping(self):
        s=bytearray(self.scenes[0]);s[:2048]=bytes(2048);s[0]=1;s[2404]=1
        s[2048:2052]=bytes((0,1,0,16))
        asset=pack([bytes(s)],self.tiles,self.palette,[{'frame':0,'world':1,'level':1}],100000)
        self.assertTrue(self.open(asset));out=C.create_string_buffer(256*240);self.lib.get_frame(out)
        self.assertEqual(out.raw[:8],bytes([1])*8)
        self.assertEqual(out.raw[256+16:256+24],bytes([17])*8)
        s[2347]=24 # background and sprite left clipping
        asset=pack([bytes(s)],self.tiles,self.palette,[{'frame':0,'world':1,'level':1}],100000)
        self.assertTrue(self.open(asset));self.lib.get_frame(out);self.assertEqual(out.raw[:8],bytes(8))

    def test_header_index_integrity_and_truncations(self):
        for length in (0,4,12,63,64,200,len(self.asset)-1): self.assertFalse(self.open(self.asset[:length]))
        for offset in (4,12,14,16,20,24,26,28,30,32,36,40,44,48,52,8384,8388,8392,8394,8396):
            b=bytearray(self.asset);b[offset]^=128
            if offset==20:struct.pack_into('<I',b,20,1)
            struct.pack_into('<I',b,8,binascii.crc32(b[12:]))
            self.assertFalse(self.open(b),offset)
        rng=random.Random(203)
        for _ in range(500):
            b=bytearray(self.asset);b[rng.randrange(12,len(b))]^=1<<rng.randrange(8)
            self.assertFalse(self.open(b))

    def test_malformed_compressed_scene_fails_closed(self):
        s=bytearray(self.scenes[0]);s[2404]=9
        self.assertFalse(self.open(pack([s],self.tiles,self.palette,[{'frame':0,'world':1,'level':1}],100000)))
        s[2404]=1;s[2304]=64
        self.assertFalse(self.open(pack([s],self.tiles,self.palette,[{'frame':0,'world':1,'level':1}],100000)))

if __name__=='__main__':unittest.main()
