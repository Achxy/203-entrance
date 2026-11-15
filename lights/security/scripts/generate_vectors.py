#!/usr/bin/env python3
"""Offline interoperability fixtures; all credentials/private scalars are TEST ONLY.

Uses the pinned ESP-IDF Python SRP client and protobuf definitions, independently
checks the native C server formulas, and uses cryptography for AES-GCM/P-256.
No device, account, Wi-Fi, or network access occurs.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

ROOT = Path(__file__).resolve().parents[3]
IDF = ROOT / ".tools/esp-idf"
PIN = "fff9895c82d744c7237be8847347bdd1b07c6643"
actual = subprocess.check_output(["git", "-C", str(IDF), "rev-parse", "HEAD"], text=True).strip()
if actual != PIN:
    raise SystemExit(f"Expected pinned IDF {PIN}; got {actual}")
os.environ["IDF_PATH"] = str(IDF)
sys.path.insert(0, str(IDF / "examples/protocols/esp_local_ctrl/scripts"))
import proto  # noqa: E402
from security import srp6a  # noqa: E402
from security.security2 import Security2  # noqa: E402


def raw(n):
    return n.to_bytes((n.bit_length() + 7) // 8, "big") or b"\0"


def sha512(b):
    return hashlib.sha512(b).digest()


def digest(*records):
    return hashlib.sha256(b"".join(struct.pack(">I", len(r)) + r for r in records)).digest()


N, g = srp6a.get_ng(srp6a.NG_3072)
k = int.from_bytes(sha512(raw(N) + g.to_bytes(384, "big")), "big")
username, password = "203-guest", "not-a-real-lighting-password"
salt = bytes.fromhex("036ee0c7bcb9eda84c9eac97d93decf4f4aa")
x = int.from_bytes(sha512(salt + sha512((username + ":" + password).encode())), "big")
v = pow(g, x, N)
signing_key = ec.derive_private_key(0x203CA7, ec.SECP256R1())  # TEST ONLY
spki = signing_key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
challenge = hashlib.sha256(b"203-lights fixed test challenge").digest()
boot = "0123456789abcdef"
gateway = "203-c7f088"
nonce = bytes.fromhex("8091a2b3c4d5e6f700000001")


def vector(name, a, b):
    A = pow(g, a, N).to_bytes(384, "big")
    B = raw((k * v + pow(g, b, N)) % N)
    u = int.from_bytes(sha512(A + B.rjust(384, b"\0")), "big")
    # Independent server-side formula, matching esp_srp.c's minimal S serialization.
    S = pow((int.from_bytes(A, "big") * pow(v, u, N)) % N, b, N)
    K = sha512(raw(S))
    hxor = bytes(a ^ b for a, b in zip(sha512(raw(N)), sha512(g.to_bytes(384, "big"))))
    M = sha512(hxor + sha512(username.encode()) + salt + A + B + K)
    HAMK = sha512(A + M + K)

    # Run the unchanged pinned IDF Python client with a deterministic test scalar.
    old_random = srp6a.get_random_of_length
    srp6a.get_random_of_length = lambda _: a
    try:
        python_client = srp6a.Srp6a(username, password)
        pyproof = python_client.process_challenge(salt, B)
        assert python_client.K == K
        if A[0] != 0:
            assert pyproof == M
            python_client.verify_session(HAMK)
            assert python_client.authenticated()
    finally:
        srp6a.get_random_of_length = old_random

    c0 = proto.session_pb2.SessionData(sec_ver=proto.session_pb2.SecScheme2)
    c0.sec2.msg = proto.sec2_pb2.S2Session_Command0
    c0.sec2.sc0.client_username, c0.sec2.sc0.client_pubkey = username.encode(), A
    r0 = proto.session_pb2.SessionData(sec_ver=proto.session_pb2.SecScheme2)
    r0.sec2.msg = proto.sec2_pb2.S2Session_Response0
    r0.sec2.sr0.device_salt, r0.sec2.sr0.device_pubkey = salt, B
    c1 = proto.session_pb2.SessionData(sec_ver=proto.session_pb2.SecScheme2)
    c1.sec2.msg = proto.sec2_pb2.S2Session_Command1
    c1.sec2.sc1.client_proof = M
    r1 = proto.session_pb2.SessionData(sec_ver=proto.session_pb2.SecScheme2)
    r1.sec2.msg = proto.sec2_pb2.S2Session_Response1
    r1.sec2.sr1.device_proof, r1.sec2.sr1.device_nonce = HAMK, nonce
    records = [p.SerializeToString() for p in (c0, r0, c1, r1)]
    transcript = digest(*records)
    context = f"203-lights-channel-v1\n{gateway}\n{boot}\n{transcript.hex()}\n{challenge.hex()}\nguest\n".encode()
    signature = signing_key.sign(context, ec.ECDSA(hashes.SHA256(), deterministic_signing=True))
    binding_response = json.dumps(dict(v=1, op="bound", gateway_id=gateway, boot=boot,
                                      transcript=transcript.hex(), challenge=challenge.hex(),
                                      role="guest", signature=signature.hex()), separators=(",", ":")).encode()
    aes = AESGCM(K[:32])

    def enc(count, value):
        return aes.encrypt(nonce[:8] + struct.pack(">I", count), value, None)

    request, response = b'{"v":1,"op":"status"}', b'{"v":1,"ok":true}'
    # Check patched Python nonce advancement against independent AES-GCM records.
    pc = Security2(1, username, password, False)
    pc.cipher, pc.nonce = aes, bytearray(nonce[:8] + struct.pack(">I", 3))
    assert pc.encrypt_data(request) == enc(3, request)
    assert pc.decrypt_data(enc(4, response)) == response
    assert bytes(pc.nonce) == nonce[:8] + struct.pack(">I", 5)

    return dict(name=name, username=username, password=password, ephemeral=raw(a).rjust(32, b"\0").hex(),
                request0=records[0].hex(), response0=records[1].hex(), request1=records[2].hex(), response1=records[3].hex(),
                transcript=transcript.hex(), key=K[:32].hex(), nonce=nonce.hex(),
                gateway=gateway, boot=boot, role="guest", spki=spki.hex(), challenge=challenge.hex(),
                binding_context=context.hex(), binding_response=binding_response.hex(),
                binding_ciphertext=enc(2, binding_response).hex(), plaintext_request=request.hex(),
                plaintext_response=response.hex(), request_ciphertext=enc(3, request).hex(),
                response_ciphertext=enc(4, response).hex(), next_response_ciphertext=enc(6, response).hex(),
                client_public_key_bytes=len(raw(int.from_bytes(A, "big"))), server_public_key_bytes=len(B),
                shared_secret_bytes=len(raw(S)),
                python_proof_checked=A[0] != 0)


def candidate(label, i):
    return int.from_bytes(hashlib.sha256(f"203 fixture {label} {i}".encode()).digest(), "big")


a, b = candidate("a", 0), candidate("b", 0)
assert len(raw(pow(g, a, N))) == 384
normal = vector("normal", a, b)
for i in range(1, 10000):
    short_a = candidate("a", i)
    if len(raw(pow(g, short_a, N))) < 384:
        break
else:
    raise RuntimeError("No leading-zero A fixture found")
for i in range(1, 10000):
    short_b = candidate("b", i)
    if len(raw((k * v + pow(g, short_b, N)) % N)) < 384:
        break
else:
    raise RuntimeError("No short B fixture found")
vectors = [normal, vector("leading_zero_A", short_a, b), vector("short_B", a, short_b)]

# Verify supplied ESP-IDF native test_srp.c salt/verifier fixture independently.
test_c = (IDF / "components/protocomm/test_apps/main/test_srp.c").read_text()
def c_array(name):
    content = re.search(r"static const char " + name + r"\[\] = \{(.*?)\};", test_c, re.S)[1]
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", content))
native_salt, native_v = c_array("sec2_salt"), c_array("sec2_verifier")
native_x = int.from_bytes(sha512(native_salt + sha512(b"wifiprov:abcd1234")), "big")
assert raw(pow(g, native_x, N)) == native_v

# Unlike upstream Python's intermediate integer conversion, the native C server
# hashes all 64 inner-digest bytes, including any leading zero. This regression
# fixture also retains a leading zero in the fixed-width salt.
zero_salt = bytes.fromhex("00112233445566778899aabbccddeeff")
zero_user = "203-guest"
for i in range(10000):
    zero_password = f"zero-inner-digest-fixture-{i}"
    inner = sha512((zero_user + ":" + zero_password).encode())
    if inner[0] == 0:
        break
else:
    raise RuntimeError("No leading-zero inner-digest fixture")
zero_x = int.from_bytes(sha512(zero_salt + inner), "big")
zero_verifier = pow(g, zero_x, N).to_bytes(384, "big")
assert zero_x != srp6a.calculate_x(hashlib.sha512, zero_salt, zero_user, zero_password)

output = Path(__file__).resolve().parents[1] / "Tests/LightsSecurityTests/Fixtures/idf61.json"
output.write_text(json.dumps(dict(idf_revision=PIN, native_verifier_matched=True,
                                 verifier_fixture=dict(username="wifiprov", password="abcd1234", salt=native_salt.hex(), verifier=native_v.hex()),
                                 leading_zero_verifier_fixture=dict(username=zero_user, password=zero_password, salt=zero_salt.hex(),
                                                                    inner_digest=inner.hex(), verifier=zero_verifier.hex()),
                                 vectors=vectors), indent=2) + "\n")
print(f"Generated {len(vectors)} fixtures; pinned Python SRP/AES-GCM and native verifier checks passed.")
