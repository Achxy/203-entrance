# LightsCore

A dependency-free native Swift WiZ codec for macOS 14 and later. It uses
Foundation and CryptoKit and is separate from the production display firmware.
It does not open sockets, scan Bluetooth, join Wi-Fi, provision/reset bulbs,
store credentials, or grant guest/owner permissions.

The protocol behavior and signing test vector were checked against
[`sbidy/pywizlight`](https://github.com/sbidy/pywizlight) at commit
[`19e607afe0eb6db1473602639e194d691234d4fd`](https://github.com/sbidy/pywizlight/tree/19e607afe0eb6db1473602639e194d691234d4fd),
specifically `utils.py`, `bulb.py`, `tests/test_signing.py`, and
`tests/test_bulb_min_dim_level.py`. Its MIT notice is included in
`LICENSE-pywizlight.txt`.

## API

- `WiZMAC`: normalized 12-hex identity, accepting raw, colon-separated or
  hyphen-separated input. Mixed separators, whitespace and non-ASCII hex fail.
- `WiZCapabilities`: explicit reported feature/range information. Unknown
  features are disabled; there is no assumption that every bulb supports RGB,
  CCT, scenes, metering, or dual zones.
- `WiZLightingPatch`: power, brightness percent, raw RGB/white channels,
  CCT, scene ID, effect speed and dual-zone ratio. Encoding validates both
  field combinations and the selected bulb's capabilities.
- `WiZRequestEncoder`: bounded compact `getPilot`, `getSystemConfig`,
  `getModelConfig`, `getUserConfig`, `getPower` and `setPilot` requests.
- `WiZResponseDecoder`: typed `getPilot` state and `setPilot` acceptance
  results, with strict method/identity/type/range checks.
- `StrictJSONValidator.validate(_:maximumBytes:)`: bounded strict syntax and
  decoded-key uniqueness checks for gateway payloads before schema decoding.
  This performs no authentication and does not infer a reply schema.

```swift
import LightsCore

let capability = try WiZCapabilities(
    brightness: true, minimumDimming: 10,
    rgb: true, kelvinRange: 2200...6500,
    supportedSceneIDs: [1, 4, 11], effectSpeedRange: 10...200
)
let patch = WiZLightingPatch(power: true, brightnessPercent: 50,
                             colorTemperature: 2700)
// The owner imports this from WiZ's local-integration home export. Real keys
// belong in the trusted gateway/owner credential store, never a shipped app.
let key = try WiZHomeKey(hex: importedHomeKey)
let request = try WiZRequestEncoder.setPilot(
    patch, capabilities: capability,
    security: .verifiedOnly(homeKey: key),
    unixTime: trustedUnixSeconds, clockIsTrusted: true
)
```

The UI/gateway must supply capabilities based on the enrolled bulb's actual
configuration. This package deliberately has no guessed module-name inference
or universal scene list. Request construction does not send the packet.

## Compatibility and security rules

**Signed requests.** `.verifiedOnly(homeKey: nil)` fails. Missing/untrusted time
also fails. There is no automatic unsigned retry. The 16-byte home key is
decoded from exactly 32 hexadecimal digits. HMAC-SHA256 covers the exact compact
`params` bytes, including `sigTs`, that are embedded in the wire request. Their
field order is fixed; no dictionary reserialization can change the signed bytes.
The caller supplies a trusted clock. Timestamp acceptance windows are firmware
dependent, so the library does not assert a universal 30- or 60-second window.

**Legacy control.** `.legacyUnsigned` is an explicit enrollment policy, not a
fallback after signature failure. Legacy bulbs must be on a protected local
network where guests cannot bypass the gateway's authorization.

**Dimming.** This API uses percent. Positive values below the reported
`minDimLevel` are raised to that minimum, and
`WiZEncodedRequest.effectiveBrightnessPercent` exposes the effective result.
The default minimum of 10 matches pywizlight's fallback; bulbs reporting 1 may
use 1. Unlike pywizlight's low-level 0–255 `PilotBuilder`, a zero-percent app
request means explicit `state:false`, with no `dimming:0` packet. Combining zero
with a powered-on/style request fails. A raw RGB request sends the supplied
channels directly; it does not perform pywizlight's optional RGB-to-white
extraction or claim calibrated color fidelity.

**Replies are evidence, not authority.** Every decoded response has
`trust == .unverifiedLAN`. The presence of `env:"pro"` or `hmac` does not make a
reply authenticated. Response-HMAC verification is not implemented, because
the pinned upstream request-signing support is not a complete verified receive
contract for the actual bulbs. A future transport must check the source peer,
bind the enrolled MAC, constrain the local network and reconcile state. The
WiZ UDP protocol does not provide an exactly-once command ID in these messages.
Gateway authorization, authenticated BLE/Internet envelopes, retries,
idempotency, freshness, owner precedence and revocation belong above this codec.

`WiZAcknowledgement.accepted` only means a matching `setPilot` reply reported
`success:true`; it never means the light visibly changed. Read `getPilot` after
the write and compare the reported fields with the effective requested fields.
Physical confirmation remains a separate hardware acceptance check.

**Bounded parsing.** Replies are limited to 16 KiB, 12 nested container levels
and 1,024 JSON values. Duplicate keys (including escaped equivalent names),
trailing JSON, malformed strings, boolean/number coercions, ambiguous simultaneous
result/error envelopes, partial RGB and out-of-range fields are rejected.
Bounded unknown extension fields are tolerated for forward compatibility.

## Validation

From this directory:

```sh
CLANG_MODULE_CACHE_PATH="$PWD/.cache/clang" \
SWIFTPM_MODULECACHE_OVERRIDE="$PWD/.cache/swift" \
swift run --cache-path .cache --scratch-path .build --disable-sandbox LightsCoreChecks
```

The dependency-free executable harness works with macOS Command Line Tools,
which do not include XCTest or Swift Testing on the development machine.
Its 14 check groups exercise the exact upstream signing vector, authentication failures,
capability limits, color-mode conflicts, dimming policy, typed state/acceptance
responses, identity mismatch, duplicate keys, malformed packets and parser
bounds. All build/cache outputs are locally ignored. These tests are host
protocol checks, not evidence of commissioning, bulb control, radio reliability,
or a complete distributable menu-bar application.
