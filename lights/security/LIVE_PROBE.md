# Read-only native gateway probe

`LightsGatewayProbe` uses the real `GatewayAdapter` and compiled
`Room203Defaults` public identity pin. No arguments shows help. The Internet
route remains a command-line operation. Bluetooth qualification uses a separate
native `203 Lights Probe.app`, explicit macOS approval, bounded service discovery,
then a separately selected peripheral and authenticated read. There are no bulb
control, policy, pairing, provisioning, guest-enrollment or endpoint-override
operations. It never changes the Mac's Wi-Fi network.

## Offline validation and build

```sh
bash lights/security/scripts/check-probe.sh
lights/macos/.build/release/LightsGatewayProbe --help
```

The optimized offline runner passed **111 assertions on 2026-11-16**, using only
synthetic bundles and temporary files. Checks cover explicit modes and bounds,
the app/permission boundary before credential loading, refusal to request
approval during discovery or authentication, forced Bluetooth bypassing Internet
readiness, matching-service discovery limits, exact bundle schema/target,
duplicate keys, permissions, symlink/hardlink/FIFO/oversized-file rejection,
credential clearing, evidence redaction, route-specific identity requirements
and exclusive 0600 evidence creation. These checks do not instantiate a Bluetooth
manager or query the real privacy permission state. No private owner bundle,
Keychain, network, radio or hardware is accessed.

Build the diagnostic app independently of the universal menulet:

```sh
bash lights/macos/scripts/build-probe.sh
```

The script requires the already-resolved pinned Swift dependencies. It creates a
new `lights/macos/build/probe/qualification.XXXXXX/203 Lights Probe.app`, checks
the property list and ad-hoc signature, and runs that executable's offline
`--check` mode. Its sibling `manifest.json` records the executable SHA-256 and
designated code requirement. It does not open the app, prompt, scan or replace
any existing app. This host-architecture development app is not notarized or a
distribution artifact.

The 2026-11-16 arm64 package passed property-list validation, strict code-signature
verification and the same 111 assertions from inside its signed bundle. Its
no-argument invocation printed help and exited. Permission presentation and
radio use were not part of packaging validation.

## Explicit Internet read

After the owner has qualified and activated the intended firmware, invoke:

```sh
lights/macos/.build/release/LightsGatewayProbe --live \
  --bundle /absolute/private/owner-access.json \
  --evidence /absolute/private/new-probe-evidence.json \
  --transport internet --timeout 45
```

This route performs the bounded HTTPS readiness check and uses the Internet
relay only. It does not query Bluetooth authorization or open a Bluetooth
manager.

## Explicit Bluetooth qualification

Use the **same frozen diagnostic app** for all three steps in the logged-in
macOS GUI session. Replace the example app path with the absolute path emitted
by the packaging script. Each evidence path must be new; keep these private.
Launch Services is used so the native application is the permission identity.
Do not run a raw Swift executable for Bluetooth or assume it inherits the
menulet's permission.

1. Request permission alone. This invocation rejects owner bundles, does not
   scan or connect, and may present the standard macOS Bluetooth dialog. The
   operator must approve it if access is not already authorized. Denied or
   restricted access fails; the tool never resets or edits macOS permissions.

   ```sh
   open -n -W "/absolute/qualification.XXXXXX/203 Lights Probe.app" --args \
     --authorize-bluetooth --evidence /absolute/private/new-permission.json
   ```

2. After permission succeeds, discover only the lighting service. This rejects
   owner bundles and requires existing authorization; it cannot request initial
   approval. The scan is filtered to service UUID
   `89f5e330-0ba9-4d7d-b7e4-320303000001`, runs for ten seconds by default and
   retains at most eight matching peripherals. Private evidence contains only
   their Core Bluetooth UUIDs and available RSSI values, with
   `identity_verified: false`. Advertised names, MAC addresses and manufacturer
   data are not retained. Advertising the UUID does not authenticate a gateway.

   ```sh
   open -n -W "/absolute/qualification.XXXXXX/203 Lights Probe.app" --args \
     --discover-bluetooth --scan-seconds 10 \
     --evidence /absolute/private/new-discovery.json
   ```

3. Select the intended peripheral UUID explicitly from the private discovery
   evidence. The live read checks the signed diagnostic app identity and
   existing Bluetooth permission **before opening the owner bundle**, then
   connects only to that peripheral. It completes the same SRP and pinned P-256
   transcript binding as Internet mode. It forces Bluetooth: no Internet
   readiness request, socket or fallback is used. A wrong device fails the
   identity check.

   ```sh
   open -n -W "/absolute/qualification.XXXXXX/203 Lights Probe.app" --args \
     --live --transport bluetooth --peripheral SELECTED-PERIPHERAL-UUID \
     --bundle /absolute/private/owner-access.json \
     --evidence /absolute/private/new-bluetooth-read.json --timeout 45
   ```

Inspect the evidence's `operation`, `outcome`, `error_code` and, for a room read,
`identity_verified`. `open -W` waits for app exit; its own exit status is not the
probe's authenticated result. A successful permission or discovery operation is
not an authenticated room read. The app intentionally has no general command
console, auto-selection or automatic retry. Its native process exits after the
bounded operation and cleanup.

Permission wait defaults to 120 seconds (allowed 5–180). Discovery defaults to a
10-second scan (1–15) inside a 20-second overall deadline (5–30, strictly longer
than the scan). `--timeout` sets these deadlines per mode. A powered-off or
unavailable Bluetooth controller fails discovery. Permission mode only reports
privacy authorization; it does not assert that the radio is on or that the
gateway is present.

The app has its own identifier, `dev.achyuth.203lights.probe`, and
`NSBluetoothAlwaysUsageDescription`, as required by Apple's
[Bluetooth purpose-string documentation](https://developer.apple.com/documentation/bundleresources/information-property-list/nsbluetoothalwaysusagedescription).
The permission gate uses the documented
[authorization check before creating a manager](https://developer.apple.com/documentation/corebluetooth/cbmanager/authorization-swift.type.property).
Apple documents that an ad-hoc signature's identity is tied to a particular
code version, so rebuilding can require new approval; a constant bundle ID
alone does not preserve it. Keep the manifest and app unchanged during
qualification. See
[TN3127: code signing requirements](https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements).
This development packaging does not bypass TCC or grant sandbox entitlements.

## Credential and evidence guarantees

The bundle must match the v1 file generated by `lights/tools/access.py`, belong to
the current user, have exact permissions 0600, be a regular file with one link,
and be no larger than 8 KiB. The final path cannot be a symlink. Its gateway ID
must match the compiled Room 203 ID. The credential is consumed in-process and
never passed in command-line arguments, URLs or environment variables. No
Keychain credential operation is permitted; an injected store throws if the
adapter unexpectedly tries one. Swift/Foundation copies still prevent a complete
memory-zeroization guarantee.

Every evidence file must not already exist. It is reserved with `O_EXCL`, written
with permissions 0600 and flushed. It contains the public key fingerprint,
authenticated boot/owner role, duration, aggregate room observation/policy counts
and a bounded error classification. It excludes credentials, salts, verifiers,
personal guest labels, grant identifiers, light names, MAC addresses and raw peer
errors. Bluetooth evidence additionally records the checked app identifier and
binary fingerprint, permission status, and selected or discovered peripheral
UUIDs. Existing evidence or bundle files are never overwritten.

Live room reads complete real SRP plus pinned P-256 channel binding, read
room/light/session pages, and close the connection. The default overall live
deadline is 45 seconds, configurable from 5 to 120; task cancellation closes the
transport and normal failure paths produce sanitized failure evidence. Crypto
operations are cooperative, and each network operation also has its own bounded
deadline. An abrupt process termination can leave an empty evidence file, which
is not a successful result. A previous owner connection that did not close may
occupy the sole owner slot until its 60-second idle deadline.

The native executable's exit 0 means its selected operation completed, exit 1
means the attempt failed and failure evidence was written, and exit 2 means
arguments or evidence setup failed. A successful live read is evidence of this
owner session and room read only. It does not prove
physical light control, commissioning, signature enforcement by bulbs, display
coexistence, heap stability, or a sustained Internet connection.

The BLE extension has been built and tested offline. Actual macOS permission
presentation, radio discovery, BLE authentication and hardware coexistence are
separate qualification steps; none is claimed by the offline runner.
