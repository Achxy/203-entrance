# Isolated resource probe

This project measures link/image cost for ESP-IDF v6.1 on ESP32-C3. It does not
implement a usable light gateway. It must not be flashed to the production
display. No device has been flashed with it.

`PROBE_STAGE=0` links NVS and heap logging, `1` adds a peripheral-only NimBLE
service, `2` adds two separate Security2 instances, and `3` links the current
three-entry light-session manager's complete public API. All stages use the same
configuration so the second and third builds reuse compiled IDF objects. The
Security2 salt/verifier are deliberately invalid public fixtures. Every GATT
request is denied. There is no provisioning, network configuration or bulb control.

The probe uses two connections, MTU 128, six 128-byte and six 256-byte MSYS blocks,
and smaller transport pools. These settings are a measurement hypothesis, not
validated flow-control or performance defaults. No Security2 handshake has been
executed. The minimal standalone service has no application fragmentation; a
384-byte SRP public key needs a separate bounded transport at this MTU.

Use `measure.py` to build stages with the repository's pinned toolchain into
ignored `.local/lights-resource-probe/build`, preserving the production build.
It never invokes flash, monitor, serial, Bluetooth or network configuration.

Results are standalone link costs. Neither an ELF nor unused runtime heap logging
proves actual initialization memory, authentication peak, multi-client behavior,
Wi-Fi coexistence, lighting capacity or display timing.

`combined.py` snapshots the production application into the ignored experiment
directory and builds successive configuration profiles with a disposable signing
key. It retains the current lighting authority/session API alongside the display
application. `--with-transport` links the real bounded BLE transport;
`--with-gateway` also links gateway persistence, the WiZ driver and the complete
service/adapter objects. The latter replaces the standalone authority object so
its RAM is not double-counted. It does not include the final runtime worker or
prove any live connection. All generated images are non-deployable experiments.

```sh
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/firmware/probe/combined.py --with-controller-flash
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python lights/firmware/probe/combined.py --with-transport --with-gateway --only wifi-bt-suspend
```

The controller-in-flash profile without flash auto-suspend is useful only for
measuring a size delta; IDF warns BLE can fail during flash erasure. Even the
auto-suspend profile requires confirming flash support and physical erase/write
qualification before deployment. Source snapshots, hashes, resolved configs,
signed sizes, maps and logs are retained per profile. The scripts never invoke
flash, monitor or device/network operations.

See [the resource audit](../../../docs/lights/resource-budget.md) for actual
measurements, heap limitations and proposed qualification gates.
