"""Release format shared by the CLI and host tests. No device credentials here."""
from __future__ import annotations

import hashlib
import json
import re
import struct
from pathlib import Path

BOARD = "esp32c3-supermini-4m"
PROJECT = "203-entrance"
SLOT_BYTES = 0x1E0000
MAGIC = 0x32303345
IDF_COMMIT = "fff9895c82d744c7237be8847347bdd1b07c6643"
RELEASE_REPO = "Achxy/203-entrance-firmware"
VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?\Z")


def validate_version(value: str) -> str:
    match = VERSION.fullmatch(value)
    if len(value) > 31 or not match:
        raise ValueError("Version must be SemVer, at most 31 ASCII characters")
    if any(int(match[i]) > 0xFFFFFFFF for i in (1, 2, 3)):
        raise ValueError("Version major/minor/patch must fit unsigned 32-bit integers")
    if match[4] and any(p.isdigit() and len(p) > 1 and p[0] == "0" for p in match[4].split(".")):
        raise ValueError("Numeric prerelease identifiers must not have leading zeros")
    return value


def c_string(data: bytes) -> str:
    if b"\0" not in data:
        raise ValueError("Unterminated image descriptor string")
    return data.split(b"\0", 1)[0].decode("ascii")


def image_metadata(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 4096 or len(data) > SLOT_BYTES or len(data) % 4096:
        raise ValueError("Signed application must fit a slot and include its 4096-byte signature sector")
    if data[0] != 0xE9 or struct.unpack_from("<H", data, 12)[0] != 5:
        raise ValueError("Image is not an ESP32-C3 application")
    if struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise ValueError("Application description is missing")
    version = validate_version(c_string(data[48:80]))
    if c_string(data[80:112]) != PROJECT:
        raise ValueError("Wrong application project")
    magic, layout, schema_min, schema_max, board, descriptor_version = struct.unpack_from("<IIII32sI", data, 288)
    if (magic, layout, descriptor_version, c_string(board)) != (MAGIC, 1, 1, BOARD) or not schema_min <= 1 <= schema_max:
        raise ValueError("Incompatible board, partition layout, or settings schema")
    if data[-4096:-4094] != b"\xe7\x02":
        raise ValueError("Secure Boot v2 signature block is missing")
    return {"format": 1, "version": version, "board": BOARD, "layout": layout,
            "schema": 1, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def manifest_bytes(metadata: dict) -> bytes:
    validate_manifest(metadata)
    return (json.dumps(metadata, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")


def validate_manifest(value: dict) -> dict:
    if not isinstance(value, dict) or set(value) != {"format", "version", "board", "layout", "schema", "size", "sha256"}:
        raise ValueError("Unexpected manifest fields")
    if any(type(value[k]) is not int for k in ("format", "layout", "schema", "size")):
        raise ValueError("Manifest integer fields must be integers")
    if (value["format"], value["board"], value["layout"], value["schema"]) != (1, BOARD, 1, 1):
        raise ValueError("Unsupported manifest target or format")
    validate_version(value["version"])
    if not 4096 <= value["size"] <= SLOT_BYTES or value["size"] % 4096:
        raise ValueError("Invalid signed image size")
    if not isinstance(value["sha256"], str) or not re.fullmatch("[0-9a-f]{64}", value["sha256"]):
        raise ValueError("Invalid image digest")
    return value


def load_manifest(data: bytes) -> dict:
    if len(data) > 4096:
        raise ValueError("Manifest exceeds size limit")

    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate manifest field")
            result[key] = value
        return result

    return validate_manifest(json.loads(data, object_pairs_hook=unique))


def sign_manifest(data: bytes, key_path: Path) -> bytes:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
    if key.key_size != 3072:
        raise ValueError("Release signing requires RSA-3072")
    return key.sign(data, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())


def verify_manifest(data: bytes, signature: bytes, public_pem: bytes) -> dict:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    key = serialization.load_pem_public_key(public_pem)
    if key.key_size != 3072 or len(signature) != 384:
        raise ValueError("Expected an RSA-3072 signature")
    key.verify(signature, data, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
    return load_manifest(data)
