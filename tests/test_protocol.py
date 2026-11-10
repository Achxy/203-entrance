"""Exercise the exact C parser used by device API and release checks."""
import ctypes
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class Manifest(ctypes.Structure):
    _fields_ = [('version', ctypes.c_char * 32), ('hash', ctypes.c_uint8 * 32), ('size', ctypes.c_size_t)]

class ProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        output = Path(cls.temp.name) / 'protocol.so'
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        if not cjson.exists():
            raise unittest.SkipTest('Run the pinned firmware build to resolve cJSON first')
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-DCJSON_NESTING_LIMIT=8', '-I', str(cjson),
                        '-I', str(ROOT / 'components/entrance/include'),
                        str(ROOT / 'components/entrance/core.c'),
                        str(ROOT / 'components/entrance/protocol.c'), str(cjson / 'cJSON.c'),
                        '-lm', '-o', str(output)], check=True)
        cls.lib = ctypes.CDLL(str(output))
        cls.lib.protocol_manifest.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(Manifest)]
        cls.lib.protocol_manifest.restype = ctypes.c_bool
        cls.lib.protocol_release_version_allowed.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        cls.lib.protocol_release_version_allowed.restype = ctypes.c_bool
        cls.lib.protocol_object.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        cls.lib.protocol_object.restype = ctypes.c_void_p
        cls.lib.cJSON_Delete.argtypes = [ctypes.c_void_p]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check(self, raw):
        result = Manifest()
        return self.lib.protocol_manifest(raw, len(raw), ctypes.byref(result)), result

    def value(self):
        return dict(format=1, version='1.2.3', board='esp32c3-supermini-4m', layout=1,
                    schema=1, size=8192, sha256='ab' * 32)

    def test_signed_manifest_payload(self):
        valid, parsed = self.check(json.dumps(self.value()).encode() + b'\n')
        self.assertTrue(valid)
        self.assertEqual(parsed.version, b'1.2.3')
        self.assertEqual(parsed.size, 8192)
        self.assertEqual(bytes(parsed.hash), bytes.fromhex('ab' * 32))

    def test_ambiguous_or_malformed_requests(self):
        for raw in [b'{}garbage', b'{"password":"a","password":"b"}', b'{"nested":{}}',
                    b'{"password":"secret\\u0000extra"}', b'{}\x00', b'[]', b'{',
                    b'{"a":' + b'[' * 50 + b'0' + b']' * 50 + b'}']:
            with self.subTest(raw=raw):
                pointer = self.lib.protocol_object(raw, len(raw))
                if pointer:
                    self.lib.cJSON_Delete(pointer)
                self.assertFalse(pointer)

    def test_incompatible_and_noncanonical_manifest(self):
        for key, values in dict(format=[2, True], layout=[0, 2], schema=[0, 2],
                                size=[4096, 8193, 0x1E0001, -1, 1.5],
                                version=['1.2', '01.2.3', '1.2.3-'],
                                board=['esp32', None], sha256=['00', None]).items():
            for value in values:
                with self.subTest(key=key, value=value):
                    payload = self.value(); payload[key] = value
                    self.assertFalse(self.check(json.dumps(payload).encode())[0])
        payload = self.value(); payload['extra'] = 1
        self.assertFalse(self.check(json.dumps(payload).encode())[0])

    def test_stable_checks_and_explicit_candidate_selection(self):
        payload = self.value(); payload['version'] = '1.2.3-rc.1'
        self.assertTrue(self.check(json.dumps(payload).encode())[0])
        for version, requested, allowed in [
            (b'1.2.3', None, True), (b'1.2.3-rc.1', None, False),
            (b'1.2.3-rc.1', b'', False), (b'1.2.3+build-info', None, True),
            (b'1.2.3-rc.1', b'1.2.3-rc.1', True),
            (b'1.2.3-rc.1', b'1.2.3-rc.2', False),
            (b'1.2.3', b'1.2.4', False), (b'../../outside', None, False),
            (None, None, False),
        ]:
            with self.subTest(version=version, requested=requested):
                self.assertEqual(self.lib.protocol_release_version_allowed(version, requested), allowed)

    def test_all_truncation_points(self):
        raw = json.dumps(self.value()).encode()
        for end in range(len(raw)):
            self.assertFalse(self.check(raw[:end])[0])
