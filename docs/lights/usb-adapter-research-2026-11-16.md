# External USB Wi-Fi adapter investigation

## Result and scope

The attached TP-Link adapter is accessible from a normal user process, but its
radio has **not** started successfully. It is not yet an alternative OTA path.
The signed RC17 candidate remains uninstalled; this investigation makes no new
claim about lighting, Bluetooth interoperability or the operating soak.

The Mac's built-in Wi-Fi, default route, DNS, security policy and installed
drivers were not changed. The existing production display relay independently
returned `online:true` during the investigation. A fresh certificate-pinned read
through the spare hub reported RC16, 4,133,077 ms uptime, healthy settings and
animation slot 2 actively rendering. That is a point observation, not a completed
60-minute lighting soak. The spare ESP32 remains the maintenance hub.

The next hardware gate is a real power cycle of only the external USB adapter,
followed by one bounded startup diagnostic. A software USB reset was performed
and did not reset the relevant internal radio state. The MAC-ready check must
not be bypassed, and a failed startup is not permission to upload firmware or
transmit through uninitialized hardware.

## Hardware and source provenance

- Host: Apple Silicon, macOS 26.5.2, build 25F84.
- USB: vendor `2357`, product `0120`, device revision `0200`, high-speed USB.
- The supplied 2019 Mac package contains Intel kernel drivers. It was expanded
  and inspected statically, never installed or executed.
- The inspected vendor executable is `RtWlanU`, bundle version `1830.20.b26`,
  SHA-256 `77aab600cc5dcc270e1865affeb49a52fc7f0505add3824bef51b63f7b27a991`.
- The experimental Swift driver is pinned to
  [`7ce3ba3bf33bd89e5a588526d1f822672b7dc53a`](https://github.com/Slooquie/archer-t2u-plus-macos-driver/tree/7ce3ba3bf33bd89e5a588526d1f822672b7dc53a).

The prototype is not a supported vendor driver or a distributable
Mac networking product. It uses an ordinary process and IOUSBHost rather than
installing the upstream privileged service.

## Isolated transport prototype

The local prototype exposes only a loopback TCP listener and forwards one active
connection to the production display's fixed management address. The optional
HTTP diagnostic port is separate and explicit. It creates no OS network
interface and adds no routes. Existing HTTPS certificate validation and the
independent device certificate pin remain end to end.

The Swift radio layer handles WPA2, DHCP and ARP. A separately pinned
`smoltcp 0.14.0` shim handles IPv4/TCP checksums, reassembly, retransmission and
flow control. Packet queues, stream buffers, active/waiting connections, connect
deadlines and total execution time are bounded. Link loss closes the TCP stream;
the proxy never replays accepted application bytes or an OTA request.

Offline tests cover radio receive bounds/replay gates and actual Swift socket
partial writes and close behavior. An adversarial slow-reader test exposed loss
of unread data when upstream TCP TIME_WAIT expired. A fixed 64 KiB retention
buffer now preserves the final bytes without stopping TCP timers or accepting
new application data after closure. The regression passes with a 50,321-byte
response, both close orders, partial writes and late duplicate/corrupt packets.

Completed host checks: 9 Rust tests, strict Clippy, C ABI linkage, 42 actual
Swift proxy assertions, 103 radio receive-safety assertions and the upstream
offline driver suites. Eleven executable preflight cases additionally reject
unsafe profile files, invalid network credentials and a wrong firmware image
before USB access. The arm64 `esp-link` executable builds successfully on this
Mac. Its linked Rust runtime was built for macOS 26; older macOS versions are
not supported or claimed by this local prototype. Passing host tests alone does
not establish radio reliability, association or OTA qualification.

## Startup evidence

| Experiment | Observed result |
| --- | --- |
| Read USB descriptors and register values | Succeeded without root or installation |
| Original bounded power-on and passive scan | MAC-on request timed out before firmware download or scanning |
| Open and retain USB interface 0 before power-on | Same failure; actual GET_CONFIGURATION returned 1 |
| Documented IOUSBHost device reset and re-enumeration | Reset succeeded; subsequent power-on failed identically |
| Read factory ROM before power-on, matching reference lifecycle | Valid factory ROM read; same power-on failure |

At the failed gate, `APS_FSMCO=0x14030112`, `CR=0xeaea`,
`SYS_FUNC_EN=0x501c`, `SYS_CLKR=0x3023`, and `RSV_CTRL` low byte is zero.
Power-ready and reset-release bits are set, but the MAC-on request remains set.
The loader and 8 MHz clock enables are already present.

Independent comparison with the supplied vendor binary and FreeBSD/Linux
reference paths found no justified missing pre-power register write. The
vendor's ordinary disable/reset path deliberately skips `CR=0xea`, so applying
that sequence to this state would be an additional experiment, not a proven
fix. Longer arbitrary polling, different chip-family sequences, and forced
ready-bit changes were not used.

## Private reproducibility evidence

All files below are ignored local research artifacts. They contain no required
public release assets and are not automatically installed or run:

- `.local/usb-wifi-driver/driver/`: pinned upstream checkout plus local patches.
- `.local/usb-wifi-driver/tcp-shim/`: exact dependency lockfile, C ABI and tests.
- `.local/usb-wifi-driver/*build.log`: compiler results.
- `.local/usb-wifi-driver/power-*.log` and `usb-reset-power.log`: hardware evidence.
- `.local/usb-wifi-driver/network-baseline.json`: unchanged default Internet path.
- `.local/lights-research/rtl8821au-power-on/REPORT.md`: source comparison,
  power-table decoding and disassembly evidence.
- `.local/lights-research/usb-lifecycle/usb-startup-audit.md`: independent vendor
  lifecycle and Apple USB API audit.

No Wi-Fi password, production signing key, WiZ home key or administrator token
belongs in these source artifacts or in this report.
