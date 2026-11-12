"""Exercise the actual portable firmware policy code on the host."""
import ctypes
import random
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Descriptor(ctypes.Structure):
    _fields_ = [("magic", ctypes.c_uint32), ("layout", ctypes.c_uint32),
                ("schema_min", ctypes.c_uint32), ("schema_max", ctypes.c_uint32),
                ("board", ctypes.c_char * 32), ("descriptor_version", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 3)]


class CoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        binary = Path(cls.tmp.name) / ("core.dylib" if sys.platform == "darwin" else "core.so")
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                        "-I", str(ROOT / "components/entrance/include"), str(ROOT / "components/entrance/core.c"), "-o", str(binary)], check=True)
        cls.lib = ctypes.CDLL(str(binary))
        cls.lib.version_compare.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_bool)]
        cls.lib.version_compare.restype = ctypes.c_int
        cls.lib.record_crc.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.record_crc.restype = ctypes.c_uint32
        cls.lib.hex_decode.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.hex_decode.restype = ctypes.c_bool
        cls.lib.hex_encode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        cls.lib.constant_equal.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.constant_equal.restype = ctypes.c_bool
        cls.lib.descriptor_compatible.argtypes = [ctypes.POINTER(Descriptor)]
        cls.lib.descriptor_compatible.restype = ctypes.c_bool
        cls.lib.uplink_retry_delay_ms.argtypes = [ctypes.c_bool, ctypes.c_bool, ctypes.c_bool, ctypes.c_uint64,
                                                 ctypes.POINTER(ctypes.c_bool)]
        cls.lib.uplink_retry_delay_ms.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def compare(self, a, b):
        valid = ctypes.c_bool()
        result = self.lib.version_compare(a.encode(), b.encode(), ctypes.byref(valid))
        return result, valid.value

    def test_semver_spec_order(self):
        versions = ["1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.1.0", "2.0.0"]
        for i, a in enumerate(versions):
            for j, b in enumerate(versions):
                with self.subTest(a=a, b=b):
                    self.assertEqual(self.compare(a, b), ((i > j) - (i < j), True))

    def test_build_metadata_does_not_allow_reinstall(self):
        self.assertEqual(self.compare("1.0.0+one", "1.0.0+two"), (0, True))

    def test_uplink_waits_for_trial_and_confirmation_observation(self):
        held = ctypes.c_bool(False)
        decision = self.lib.uplink_retry_delay_ms
        self.assertEqual(decision(True, False, False, 0, ctypes.byref(held)), 0)
        for uptime in range(0, 33000, 250):
            self.assertEqual(decision(True, False, True, uptime, ctypes.byref(held)), 1000)
            self.assertTrue(held.value)
        self.assertEqual(decision(True, False, False, 33000, ctypes.byref(held)), 10000)
        self.assertFalse(held.value)
        self.assertEqual(decision(True, False, False, 43000, ctypes.byref(held)), 0)

    def test_station_only_confirmer_gets_route_before_trial_deadline(self):
        held = ctypes.c_bool(False)
        decision = self.lib.uplink_retry_delay_ms
        self.assertEqual(decision(True, False, True, 44999, ctypes.byref(held)), 1000)
        self.assertEqual(decision(True, False, True, 45000, ctypes.byref(held)), 0)
        self.assertFalse(held.value)
        self.assertEqual(decision(True, False, True, 119000, ctypes.byref(held)), 0)
        self.assertEqual(decision(True, False, False, 119500, ctypes.byref(held)), 0)
        held.value = True
        self.assertEqual(decision(True, False, False, 44000, ctypes.byref(held)), 1000)
        held.value = True
        self.assertEqual(decision(True, False, False, 45000, ctypes.byref(held)), 0)

    def test_disabled_and_maintenance_uplink_never_resumes_from_trial(self):
        decision = self.lib.uplink_retry_delay_ms
        for enabled, maintenance in ((False, False), (False, True), (True, True)):
            held = ctypes.c_bool(True)
            for trial in (True, False):
                self.assertEqual(decision(enabled, maintenance, trial, 46000, ctypes.byref(held)), -1)
                self.assertFalse(held.value)

    def test_malformed_versions(self):
        for value in ["", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "1.2.3-", "1.2.3+", "1.2.3-a..b", "1.2.3-01", "1.2.3+a.", "1.2.3+a..b", "1.2.3\n", "4294967296.0.0", "1.0.0/evil", "1.0.0-" + "a" * 30]:
            with self.subTest(value=value):
                self.assertFalse(self.compare(value, "1.0.0")[1])

    def test_crc_known_vector_and_random_records(self):
        rng = random.Random(203)
        for data in [b"123456789", b"", bytes(range(256))] + [rng.randbytes(n) for n in range(1, 512, 7)]:
            self.assertEqual(self.lib.record_crc(data, len(data)), zlib.crc32(data))

    def test_hex_roundtrip(self):
        for data in [bytes(range(32)), b"\0" * 32, b"\xff" * 32]:
            output = ctypes.create_string_buffer(65)
            self.lib.hex_encode(data, len(data), output)
            self.assertEqual(output.value.decode(), data.hex())
            decoded = ctypes.create_string_buffer(32)
            self.assertTrue(self.lib.hex_decode(output.value.upper(), decoded, 32))
            self.assertEqual(decoded.raw, data)

    def test_hex_rejects_malformed_digest(self):
        target = ctypes.create_string_buffer(32)
        for value in [b"", b"0" * 63, b"0" * 65, b"z" * 64, b"-1" * 32]:
            self.assertFalse(self.lib.hex_decode(value, target, 32))

    def test_all_digest_bytes_are_compared(self):
        reference = bytes(range(32))
        self.assertTrue(self.lib.constant_equal(reference, reference, 32))
        for i in range(32):
            changed = bytearray(reference)
            changed[i] ^= 1
            self.assertFalse(self.lib.constant_equal(reference, bytes(changed), 32))

    def test_descriptor_target_and_schema(self):
        d = Descriptor(0x32303345, 1, 1, 1, b"esp32c3-supermini-4m", 1)
        self.assertTrue(self.lib.descriptor_compatible(ctypes.byref(d)))
        for name, value in [("magic", 0), ("layout", 2), ("schema_min", 2), ("schema_max", 0), ("descriptor_version", 2)]:
            previous = getattr(d, name)
            setattr(d, name, value)
            self.assertFalse(self.lib.descriptor_compatible(ctypes.byref(d)))
            setattr(d, name, previous)
        d.board = b"esp32s3"
        self.assertFalse(self.lib.descriptor_compatible(ctypes.byref(d)))


if __name__ == "__main__":
    unittest.main()
