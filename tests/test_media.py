"""Differential parsing and actual C decoding of hostile persistent display clips."""
import ctypes
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from media import inspect_clip


def clip(width=16, height=20, frames=None, fps=6):
    if frames is None:
        frames = [b"\0" + b"\x12" * ((width * height + 1) // 2)]
    data = bytearray(80)
    struct.pack_into("<4sIIBBBBHH", data, 0, b"ECL1", 0, 0, width, height, fps, 0, len(frames), 0)
    data[20:24] = b"Test"
    for frame in frames:
        data += struct.pack("<H", len(frame)) + frame
    return repair(data)


def repair(data):
    data = bytearray(data)
    struct.pack_into("<I", data, 4, len(data))
    struct.pack_into("<I", data, 8, zlib.crc32(data[12:]))
    return bytes(data)


class MediaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        target = Path(cls.tmp.name) / "media.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "components/entrance/include"),
                        str(ROOT / "components/entrance/media.c"),
                        str(ROOT / "components/entrance/core.c"), "-o", str(target)], check=True)
        cls.lib = ctypes.CDLL(str(target))
        cls.lib.media_validate.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        cls.lib.media_validate.restype = ctypes.c_bool
        cls.lib.media_frame.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t,
                                        ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p]
        cls.lib.media_frame.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def accepted(self, data):
        c = self.lib.media_validate(data, len(data), None)
        try:
            inspect_clip(data)
            python = True
        except ValueError:
            python = False
        self.assertEqual(c, python, data[:90].hex())
        return c

    def test_full_resolution_still_and_delta_playback(self):
        self.assertTrue(self.accepted(clip(128, 160)))
        raw = b"\0" + b"\x12" * 160
        delta = b"\1" + bytes([127, 0x80 | 127, 5, 63])
        data = clip(frames=[raw, delta])
        self.assertTrue(self.accepted(data))
        out, at = ctypes.create_string_buffer(10240), ctypes.c_size_t(80)
        self.assertTrue(self.lib.media_frame(data, len(data), 320, ctypes.byref(at), out))
        self.assertEqual(out.raw[:160], b"\x12" * 160)
        self.assertTrue(self.lib.media_frame(data, len(data), 320, ctypes.byref(at), out))
        self.assertEqual(out.raw[:160], b"\x12" * 64 + b"\x55" * 64 + b"\x12" * 32)
        self.assertEqual(at.value, len(data))

    def test_all_truncations_and_corrupt_checksum(self):
        data = clip()
        for n in range(len(data)):
            self.assertFalse(self.accepted(data[:n]))
        for i in range(len(data)):
            changed = bytearray(data); changed[i] ^= 1
            self.assertFalse(self.accepted(bytes(changed)))

    def test_invalid_runs_and_first_frame(self):
        raw = b"\0" + b"\0" * 160
        for frame in [b"\1\xff\x10\x7f\x3f", b"\1\xff", b"\1\x7f\x7f\x7f",
                      b"\1\x7f", b"\2", b"\0", b"\0" + b"\0" * 161]:
            self.assertFalse(self.accepted(clip(frames=[raw, frame])))
        self.assertFalse(self.accepted(clip(frames=[b"\1\x7f\x7f\x3f"])))

    def test_odd_pixel_padding_and_dimensions(self):
        self.assertTrue(self.accepted(clip(1, 1, [b"\0\x10"])))
        self.assertFalse(self.accepted(clip(1, 1, [b"\0\x11"])))
        for w, h in [(0, 20), (160, 160), (161, 1)]:
            self.assertFalse(self.accepted(clip(w, h)))

    def test_limits_and_trailing_bytes(self):
        raw = b"\0\0"
        self.assertTrue(self.accepted(clip(1, 1, [raw] * 120, 12)))
        for count, fps in [(121, 12), (61, 1), (1, 13), (1, 0)]:
            self.assertFalse(self.accepted(clip(1, 1, [raw] * count, fps)))
        self.assertFalse(self.accepted(repair(clip() + b"\0")))
        self.assertFalse(self.accepted(clip(128, 160, [b"\0" + b"\0" * 10240] * 2)))

    def test_reserved_fields_and_untrusted_titles(self):
        for offset, value in [(15, 1), (18, 1), (19, 1), (47, 1), (20, 31), (25, 65), (21, 255)]:
            data = bytearray(clip()); data[offset] = value
            self.assertFalse(self.accepted(repair(data)))

    def test_parser_differential_mutations(self):
        rng = random.Random(203)
        original = clip(frames=[b"\0" + b"\x12" * 160, b"\1\x7f\x7f\x3f"])
        for _ in range(1500):
            data = bytearray(original)
            for _ in range(rng.randrange(1, 5)):
                data[rng.randrange(len(data))] = rng.randrange(256)
            self.accepted(repair(data))
