# Gateway secure-session adapter

This is an **integration candidate**, not enabled in RC15 or in the production
build. It wraps the project's pinned ESP-IDF 6.1 Security2 implementation
(SRP-3072/SHA-512, AES-256-GCM patch1), with no Security0/1 fallback.

- Three separately owned crypto instances: at most two guest connections and
  one owner connection. Only one expensive handshake at a time; at most one
  new handshake per second. Invalid requests consume no SRP allocation.
- Exact `203-owner` and `203-guest` credential profiles. A role hint from the
  network is never authority. Store salt/verifier profiles durably; never
  compile or log passwords. Owner and WiZ signing keys are separate.
- Strict, allocation-free prevalidation of incoming protobuf before the SDK
  parser, with duplicate/unknown field, length, and wire-type checks.
- Ten-second handshake/binding deadline; sixty-second idle limit; rekey after
  65,536 request/response pairs, well before IV exhaustion.
- Exactly one encrypted request and response at a time per session. Errors,
  transport changes and lost replies destroy that session. Use a fresh SRP
  handshake and application command UUID reconciliation on failover.
- The first encrypted request proves gateway identity. A shared guest password
  by itself cannot distinguish the real gateway from another guest.

The **new application binding extension** signs:

```text
203-lights-channel-v1\n
{gateway_id}\n
{boot_16_lowercase_hex}\n
{transcript_64_lowercase_hex}\n
{client_challenge_64_lowercase_hex}\n
{owner_or_guest}\n
```

Here each `\n` denotes one LF, with no blank lines. The transcript is SHA-256 of
the exact client0, server0, client1, server1 protobuf byte strings, each prefixed
by its four-byte big-endian length. The gateway computes it itself; it never
signs a digest supplied by the client. The native app verifies the DER ECDSA
signature against a previously trusted P-256 gateway identity, its own
transcript, and fresh random challenge before sending credentials or controls.
This application extension needs end-to-end and independent security review;
it is not part of Espressif's standard provisioning protocol.

The encrypted bind request is `{v:1,op:"bind",challenge:"…"}`. Its encrypted reply
is `{v:1,op:"bound",gateway_id,boot,transcript,challenge,role,signature}`. Subsequent
application messages must also authenticate a per-install grant and enforce
the durable authority policy. A successful SRP exchange alone is not an
enrolled guest session and must not bypass individual revocation.

Run `sh check.sh` for the host parser/context suite. These tests do not execute
the IDF crypto or qualify hardware. The isolated IDF resource probe compiles
the server adapter; native/IDF cryptographic interoperability and the combined
display/lighting runtime remain release gates.

Required integration constraints:

1. Invoke all functions on a single bounded application worker, never inside
   NimBLE callbacks. Enforce source/connection rate limits before admission.
2. Check current free heap and largest block before expensive SRP admission;
   linker success is insufficient. Retain the display health threshold.
3. Cap `protocomm` and `esp_security` compilation log level at INFO or lower.
   Their upstream DEBUG statements include cryptographic secrets. Runtime log
   levels alone are not a complete safeguard.
4. Use a transport-generated route identifier; drop stale device-boot leases
   and packets after their original deadline, before calling this adapter.
5. Close guests immediately after a durable password rotation or revocation;
   recheck policy/grant validity on every application request.
6. Do not claim resistance to physical RF jamming or connection-slot flooding.
   The owner reservation protects application capacity, not the shared radio.
