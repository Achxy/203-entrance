# Gateway channel binding v1

This is a **new application extension** on top of Espressif Security2, not an upstream Espressif feature or an externally audited protocol. Its purpose is to bind the password-authenticated channel to the gateway's independently trusted P-256 device key. Shared guest credentials otherwise allow another credential holder to impersonate the SRP server.

The native app starts with a trusted descriptor containing the gateway ID and P-256 SubjectPublicKeyInfo DER bytes. Obtain it through an owner-verified invitation or an existing trusted Keychain item. Never promote a key from a relay response or BLE advertisement into trust automatically.

## Exact transcript

Let `c0`, `s0`, `c1`, and `s1` be the exact protobuf records sent and received during the successful Security2 handshake. Compute:

```text
transcript = SHA256(
    uint32BE(len(c0)) || c0 || uint32BE(len(s0)) || s0 ||
    uint32BE(len(c1)) || c1 || uint32BE(len(s1)) || s1
)
```

No extra prefix is added to this hash. Both endpoints compute it from **their own session's records**. The ESP must not accept a supplied transcript digest as the basis of a signature.

After the server's SRP proof is verified, the client generates a fresh 32-byte challenge with `SecRandomCopyBytes`. Its first AES-GCM request (nonce counter 1) is:

```json
{"v":1,"op":"bind","challenge":"<64 lowercase hex characters>"}
```

The ESP signs these exact UTF-8 bytes, including the final newline, with ECDSA P-256/SHA-256:

```text
203-lights-channel-v1\n
{gateway_id}\n
{boot_hex16}\n
{transcript_hex64}\n
{challenge_hex64}\n
{credential_role}\n
```

The displayed `\n` denotes a single LF byte, not a literal backslash. `gateway_id` is 1–48 lowercase ASCII letters, digits or hyphens. `credential_role` is exactly `owner` or `guest`, derived from the installed verifier used by this connection. It must not be taken from a client role hint. The boot identifier is eight bytes encoded as 16 lowercase hex characters.

The AES-GCM response (nonce counter 2) is a flat JSON object with **exactly** these fields:

```json
{"v":1,"op":"bound","gateway_id":"203-c7f088","boot":"0123456789abcdef","transcript":"<64 hex>","challenge":"<64 hex>","role":"guest","signature":"<P-256 DER signature, lowercase hex>"}
```

The client rejects duplicate keys, escaped aliases, unknown fields, nested values, non-ASCII strings, wrong types, malformed hex and oversized responses. It compares gateway ID, current connection boot ID, its own transcript hash, fresh challenge and expected credential role, then verifies the DER signature using the previously trusted public key. It grants no command access until every check succeeds. Subsequent application requests start at counter 3.

The boot value can initially come from the current connection's discovery metadata; the signature authenticates it before use. It is transient, not a permanent descriptor pin. A stale or incorrect boot value fails the connection; the app must discover the new boot and start a fresh handshake, without changing its trusted device key.

## Security and integration boundary

An unbound signature over only a supplied challenge can be proxied into a different SRP connection. Including the gateway's locally computed transcript prevents that substitution. A transparent relay of the complete original channel still reaches the real gateway; it does not give that relay the session key or permission to alter commands.

The extension has offline Python/CryptoKit fixtures and negative checks for each signed field, wrong key, role mismatch, malformed JSON, ciphertext replay and tampering. It still needs an independent protocol review and actual ESP-IDF end-to-end validation. The ESP firmware implementation must use its provisioned identity key, compute the transcript itself, apply its credential role, and refuse all non-bind application messages until binding succeeds. The native package cannot enforce those server-side requirements.

No automatic key replacement is supported. Losing or replacing the gateway identity requires explicit owner recovery and re-enrollment of the trusted descriptor. Distributing the application never distributes an owner credential or a trusted room's private key.
