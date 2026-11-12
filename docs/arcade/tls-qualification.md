# Certificate-chain correction — 12 November 2026

The installed RC12 resolves two relay addresses but reports an ESP-TLS
certificate-verification failure (`0x801A`, underlying `-0x3000`). It has not
established the Internet game connection.

Both public relay addresses presented the same chain during a verified
TLS 1.2 inspection from the Mac: the relay leaf, Google Trust Services WE1,
and GTS Root R4 cross-signed by GlobalSign Root CA. The pinned SDK's current
bundle contains GTS Root R4 but no longer contains that legacy GlobalSign
root. The default bundle callback follows the supplied cross-signature to
the absent legacy root. Espressif documents the same failure class and its
[cross-signed verification option](https://github.com/espressif/esp-idf/issues/18674).

A native build of the exact pinned SDK's Mbed TLS, PSA implementation and
unmodified `esp_crt_bundle.c` reproduced `-0x3000` using the captured real
chain and the firmware's generated trust bundle. Enabling
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY` accepted that chain
with zero verification flags. Wrong-hostname and expired-certificate checks
still failed. This is host cryptographic evidence, not yet an ESP connection.

The audit also found that `CONFIG_MBEDTLS_HAVE_TIME_DATE` was disabled in
RC12 and earlier configurations. Waiting for SNTP did not by itself enable
certificate expiry checks. The correction enables those checks and makes
the generated-configuration audit require expiry, the full trust bundle and
cross-signed verification. Insecure TLS, skipped verification and weak
hostname verification options are explicitly forbidden by that audit.

## Repeatable offline checks

After `./entrance setup`, run:

```sh
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python tools/check_tls_bundle.py
```

The test builds the SDK's actual certificate verifier for the host. Only
logging/platform constants are stubbed; parsing, trust selection, hashing and
signature verification use the pinned SDK implementations. Disposable test
keys generate a chain with a trusted current root and an untrusted legacy
cross-signature. No production key or live Internet service is required.

All seven cases pass: reproduction of the legacy failure, acceptance through
the trusted current root, and rejection of wrong hostname, expired,
not-yet-valid, invalid-signature and untrusted-root certificates. Time is
overridden only inside the test executable; the system clock is not changed.
CI runs these checks independently of the firmware configuration audit.

The corrective firmware still needs a signed build, healthy hardware trial,
actual relay authentication and continuous operating qualification. Do not
count these native tests as an Internet gameplay or stability pass.
