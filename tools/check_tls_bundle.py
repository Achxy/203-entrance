#!/usr/bin/env python3
"""Offline regression against the pinned SDK's actual X.509 implementation.

Creates disposable test CAs, including an untrusted legacy cross-signature.
No production key, live service, system clock change, or network is used.
"""
from datetime import datetime, timezone
import json
from pathlib import Path
import struct
import subprocess
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / '.local/tls-bundle-check'
HOST = 'display.example.test'


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def certificate(subject, issuer, public, signer, *, ca):
    builder = (x509.CertificateBuilder()
               .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject)]))
               .issuer_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, issuer)]))
               .public_key(public).serial_number(x509.random_serial_number())
               .not_valid_before(datetime(2025, 1, 1, tzinfo=timezone.utc))
               .not_valid_after(datetime(2030, 1, 1, tzinfo=timezone.utc))
               .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True)
               .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False,
                   key_encipherment=False, data_encipherment=False, key_agreement=False,
                   key_cert_sign=ca, crl_sign=ca, encipher_only=None, decipher_only=None), critical=True))
    if not ca:
        builder = builder.add_extension(x509.SubjectAlternativeName([x509.DNSName(HOST)]), critical=False)
    return builder.sign(signer, hashes.SHA256())


def bundle(cert):
    name = cert.subject.public_bytes()
    key = cert.public_key().public_bytes(serialization.Encoding.DER,
                                         serialization.PublicFormat.SubjectPublicKeyInfo)
    return struct.pack('<IHH', 4, len(name), len(key)) + name + key


def main():
    if not (ROOT / '.tools/esp-idf/components/mbedtls/mbedtls/CMakeLists.txt').exists():
        raise SystemExit('Run ./entrance setup first; this test requires the pinned SDK.')
    WORK.mkdir(parents=True, exist_ok=True)
    run('cmake', '-S', ROOT / 'tests/native/tls_bundle', '-B', WORK / 'build', '-G', 'Ninja')
    run('cmake', '--build', WORK / 'build', '-j', '4')

    keys = {name: ec.generate_private_key(ec.SECP256R1())
            for name in ('Old', 'Trusted', 'Intermediate', 'Leaf', 'Unrelated')}
    root = certificate('Trusted', 'Trusted', keys['Trusted'].public_key(), keys['Trusted'], ca=True)
    unrelated = certificate('Unrelated', 'Unrelated', keys['Unrelated'].public_key(), keys['Unrelated'], ca=True)
    cross = certificate('Trusted', 'Old', keys['Trusted'].public_key(), keys['Old'], ca=True)
    intermediate = certificate('Intermediate', 'Trusted', keys['Intermediate'].public_key(), keys['Trusted'], ca=True)
    leaf = certificate(HOST, 'Intermediate', keys['Leaf'].public_key(), keys['Intermediate'], ca=False)
    chain = [leaf, intermediate, cross]
    (WORK / 'roots.bin').write_bytes(bundle(root))
    (WORK / 'unrelated.bin').write_bytes(bundle(unrelated))
    (WORK / 'chain.pem').write_bytes(b''.join(c.public_bytes(serialization.Encoding.PEM) for c in chain))
    damaged = bytearray(leaf.public_bytes(serialization.Encoding.DER))
    damaged[-1] ^= 1  # Well-formed certificate, invalid signature.
    damaged_leaf = x509.load_der_x509_certificate(bytes(damaged))
    (WORK / 'damaged.pem').write_bytes(b''.join(c.public_bytes(serialization.Encoding.PEM)
                                               for c in [damaged_leaf, intermediate, cross]))
    current = int(datetime(2026, 11, 12, tzinfo=timezone.utc).timestamp())
    future = int(datetime(2031, 1, 1, tzinfo=timezone.utc).timestamp())
    past = int(datetime(2024, 1, 1, tzinfo=timezone.utc).timestamp())
    cases = [
        ('legacy_cross_signature_reproduces_error', 'legacy', 'roots.bin', 'chain.pem', HOST, current, False, None),
        ('trusted_cross_signature', 'cross_signed', 'roots.bin', 'chain.pem', HOST, current, True, None),
        ('wrong_hostname', 'cross_signed', 'roots.bin', 'chain.pem', 'wrong.example.test', current, False, 4),
        ('expired', 'cross_signed', 'roots.bin', 'chain.pem', HOST, future, False, 1),
        ('not_yet_valid', 'cross_signed', 'roots.bin', 'chain.pem', HOST, past, False, 512),
        ('invalid_signature', 'cross_signed', 'roots.bin', 'damaged.pem', HOST, current, False, None),
        ('untrusted_root', 'cross_signed', 'unrelated.bin', 'chain.pem', HOST, current, False, None),
    ]
    results = []
    for name, mode, roots, pem, host, now, valid, flag in cases:
        process = subprocess.run([str(WORK / 'build' / mode), str(WORK / roots),
                                  str(WORK / pem), host, str(now)],
                                 capture_output=True, text=True, check=True, timeout=15)
        result = json.loads(process.stdout)
        assert (result['result'] == 0 and result['flags'] == 0) == valid, (name, result)
        if flag is not None:
            assert result['flags'] & flag, (name, result)
        if mode == 'legacy':
            assert result['result'] == -0x3000, result
        results.append({'case': name, **result, 'passed': True})
        print(name, 'passed', flush=True)
    (WORK / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('All seven SDK certificate-chain cases passed.')


if __name__ == '__main__':
    main()
