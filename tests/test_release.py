import copy
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from firmware import BOARD, SLOT_BYTES, image_metadata, load_manifest, manifest_bytes, sign_manifest, validate_manifest, validate_version, verify_manifest
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa


class ReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.path = Path(cls.tmp.name)
        cls.key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        cls.key_path = cls.path / "key.pem"
        cls.key_path.write_bytes(cls.key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        cls.public = cls.key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def metadata(self):
        return {"format": 1, "version": "1.2.3", "board": BOARD, "layout": 1, "schema": 1, "size": 8192, "sha256": "a" * 64}

    def test_signed_manifest_roundtrip(self):
        value = self.metadata()
        data = manifest_bytes(value)
        self.assertEqual(verify_manifest(data, sign_manifest(data, self.key_path), self.public), value)

    def test_modified_manifest_signature_rejected(self):
        data = manifest_bytes(self.metadata())
        signature = sign_manifest(data, self.key_path)
        for changed in [data.replace(b"1.2.3", b"1.2.4"), data + b" ", data[:-1]]:
            with self.assertRaises(InvalidSignature):
                verify_manifest(changed, signature, self.public)

    def test_wrong_signer_rejected(self):
        other = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        public = other.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
        data = manifest_bytes(self.metadata())
        with self.assertRaises(InvalidSignature):
            verify_manifest(data, sign_manifest(data, self.key_path), public)

    def test_truncated_signature_rejected(self):
        data = manifest_bytes(self.metadata())
        with self.assertRaises(ValueError):
            verify_manifest(data, sign_manifest(data, self.key_path)[:-1], self.public)

    def test_manifest_bounds_types_and_targets(self):
        for key, bad in [("format", 2), ("layout", 2), ("schema", 2), ("board", "esp32s3"), ("size", 0), ("size", SLOT_BYTES + 4096), ("size", 4097), ("size", True), ("sha256", "z" * 64), ("version", "../firmware")]:
            value = self.metadata()
            value[key] = bad
            with self.subTest(key=key, value=bad), self.assertRaises(ValueError):
                validate_manifest(value)

    def test_duplicate_and_oversized_manifest_rejected(self):
        raw = manifest_bytes(self.metadata())
        with self.assertRaises(ValueError):
            load_manifest(raw.replace(b'{', b'{"format":1,', 1))
        with self.assertRaises(ValueError):
            load_manifest(b" " * 4097)

    def image(self):
        data = bytearray(8192)
        data[0] = 0xE9
        struct.pack_into("<H", data, 12, 5)
        struct.pack_into("<I", data, 32, 0xABCD5432)
        data[48:53] = b"1.2.3"
        data[80:92] = b"203-entrance"
        struct.pack_into("<IIII32sI", data, 288, 0x32303345, 1, 1, 1, BOARD.encode(), 1)
        data[-4096:-4094] = b"\xe7\x02"
        return data

    def test_image_descriptor_parsing(self):
        path = self.path / "image.bin"
        data = self.image()
        path.write_bytes(data)
        result = image_metadata(path)
        self.assertEqual(result["version"], "1.2.3")
        self.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())

    def test_wrong_image_targets_and_truncation(self):
        path = self.path / "bad.bin"
        for at, value in [(0, 0), (12, 9), (32, 0), (80, ord("x")), (288, 0), (292, 2), (304, ord("x")), (4096, 0)]:
            data = self.image()
            data[at] = value
            path.write_bytes(data)
            with self.subTest(at=at), self.assertRaises(ValueError):
                image_metadata(path)
        path.write_bytes(self.image()[:-1])
        with self.assertRaises(ValueError):
            image_metadata(path)

    def test_semver_validation(self):
        for value in ["0.0.1", "1.2.3-rc.1", "1.2.3+build.203"]:
            self.assertEqual(validate_version(value), value)
        for value in ["1.2.3-01", "1.2", "1.2.3+", "1.2.3/firmware", "1.2.3\n"]:
            with self.assertRaises(ValueError):
                validate_version(value)


if __name__ == "__main__":
    unittest.main()
