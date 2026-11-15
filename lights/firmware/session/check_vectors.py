#!/usr/bin/env python3
"""Check C framing against fixtures produced independently by IDF Python SRP.

This is not a hardware cryptographic test. The native Swift suite consumes the
same fixture to verify its real SRP, AES-GCM and gateway signature operations.
"""
from __future__ import annotations

import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
FIXTURES = HERE.parents[1] / "security/Tests/LightsSecurityTests/Fixtures/idf61.json"


def main() -> None:
    fixture = json.loads(FIXTURES.read_text())
    assert fixture["idf_revision"] == "fff9895c82d744c7237be8847347bdd1b07c6643"
    with tempfile.TemporaryDirectory(prefix="lights-vectors-") as directory:
        library = Path(directory) / "wire.so"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        str(HERE / "lighting_session_wire.c"), "-o", str(library)], check=True)
        wire = ctypes.CDLL(str(library))
        wire.ls_client_handshake.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint,
                                            ctypes.POINTER(ctypes.c_int)]
        wire.ls_client_handshake.restype = ctypes.c_bool
        wire.ls_binding_context.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p,
                                           ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        wire.ls_binding_context.restype = ctypes.c_size_t
        for vector in fixture["vectors"]:
            digest = hashlib.sha256()
            role = ctypes.c_int()
            for name in ("request0", "response0", "request1", "response1"):
                record = bytes.fromhex(vector[name])
                digest.update(len(record).to_bytes(4, "big"))
                digest.update(record)
                if name.startswith("request"):
                    assert wire.ls_client_handshake(record, len(record), int(name[-1]), ctypes.byref(role))
                    assert role.value == 1
                    for n in range(len(record)):
                        assert not wire.ls_client_handshake(record, n, int(name[-1]), ctypes.byref(role))
            assert digest.hexdigest() == vector["transcript"]
            output = ctypes.create_string_buffer(256)
            count = wire.ls_binding_context(output, vector["gateway"].encode(),
                                            bytes.fromhex(vector["boot"]), digest.digest(),
                                            bytes.fromhex(vector["challenge"]), 1)
            assert count and output.raw[:count].hex() == vector["binding_context"]
            print(f"PASS C / IDF-Python / Swift fixture contract: {vector['name']}")


if __name__ == "__main__":
    main()
