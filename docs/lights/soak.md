# Combined display and lighting soak

The lighting mode is opt-in and uses the existing pinned management HTTPS
connection. It does not change Wi-Fi, lighting settings, credentials or the
display. A USB-to-Wi-Fi hub may provide the HTTPS path while the Mac keeps its
Internet connection. Substitute the actual endpoint and protected device
profile, and first confirm lighting is already provisioned and active.

```sh
./entrance soak --lighting --replay --minutes 60 \
  --host 127.0.0.1:18443 --device-profile .local/device.json --tls12 \
  --output .local/lights-soak.json
```

`--replay` adds the existing requirement that replay rendering advances. Omit
it only when intentionally qualifying another display mode; an omitted replay
check is not evidence that an animation kept advancing. `--min-loops` can add a
minimum number of complete replay boundaries. `--serial --lighting` is rejected
before opening a device or writing output, because current USB health records
do not contain the required lighting telemetry.

Each ordinary iteration reads `/api/v1/status` and then `/api/v1/lights`, followed
by the existing15-second pause. Login renews after40 minutes without inserting
the normal pause between the samples around renewal. The unchanged45-second
maximum telemetry gap still applies to the complete pair of requests. A missing
lighting endpoint, expired access, inactive worker or invalid runtime telemetry
fails the run even if management and the display still report healthy.

Every accepted lighting sample requires compiled and configured firmware,
active lighting and Bluetooth, healthy storage, a running worker and a valid
worker-stack sample. Provisioning, paused, quarantined and resource-blocked
states must all be false, and `provision_result` must be `ESP_OK` (`0`). No new
stack-watermark threshold is invented. The existing management checks still
reject trials, resets, settings/display changes, stale telemetry and a settled
median heap decline greater than2 KiB.

The private JSON evidence retains only allowlisted, typed runtime fields:
those admission flags, provision result, worker stack free bytes, runtime heap
and largest block, optional session/completion counts, and local-time state and
counters. Unknown fields, credentials, verifier material and arbitrary strings
are excluded. An invalid received sample is saved as `failed_sample` without
counting it toward healthy duration. An unavailable response is recorded as an
error; the tool does not invent the missing sample. The `lighting` summary
reports observed minima for heap, largest block and valid stack watermarks,
including valid measurements from a rejected sample. Local-time readiness is
diagnostic in this mode; a clock failure is not hidden, but this check does not
require the time service to be ready or prove bulb clock recovery.

A passing zero-bulb run proves only the observed idle display/runtime/Bluetooth
coexistence window under these read requests. It does not prove active bulb
control, guest load, BLE traffic, clock recovery or bulb-state readback. For a
loaded qualification, separately record which bulbs and clients participated,
the operations performed, their acknowledged and observed outcomes, and the
matching soak interval. Session/completion counters alone do not establish
physical light behavior. These host regression tests exercise the checker;
they are not a completed hardware soak.
