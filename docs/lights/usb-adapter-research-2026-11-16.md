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

A physical adapter power cycle reproduced the failure. A second, independently
implemented driver was then tested in an isolated Linux guest, including a fresh
USB session after the owner moved the adapter to another port. It failed at the
same MAC power-on gate before a wireless interface appeared. This rules out a
Swift-only initialization defect as the sole explanation, but does not prove
that the adapter is defective: the host USB stack and hub remain shared factors.
The MAC-ready check was never bypassed.

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
| Physical adapter power cycle | Fresh cold-state register values; same MAC power-on timeout |
| Supported Linux 6.18.60 `rtw88_8821au` in a diskless guest | Exact USB ID recognized; MAC power-on poll failed; no wireless interface |
| Owner moved the adapter, followed by Linux before any Swift access | Request bit initially clear, then remained set after the driver's request; same failure |

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

## Supported-driver comparison

The Linux test used the official Alpine 3.23.8 aarch64 netboot archive. Its
SHA-256 was
`141cdd55e38ec2e7702577d43c46dab2b9078554433d77595062e590fd4d75ce`.
The detached release signature was verified in an isolated keyring against the
published primary fingerprint
`0482D84022F52DF1C4E7CD43293ACD0907D9495A`. Only the LTS kernel,
initramfs and module image were extracted. The gzip kernel payload was derived
using the [Linux zboot header layout](https://github.com/torvalds/linux/blob/v6.18/drivers/firmware/efi/libstub/zboot-header.S),
with input and output hashes retained.

QEMU 11.0.1 ran as the ordinary user with 1 GiB RAM, two virtual CPUs, a read-only
module disk, no shared folders and **no guest network adapter**. Only USB
`2357:0120` was passed through. A 120-second guest watchdog and 150-second host
deadline bounded execution. The completed diagnostic took about 13.5 seconds;
the Mac's default route and interface inventory matched before and after.
No Wi-Fi scan, association, DHCP, production credentials or OTA request occurred
through the external adapter. The VM shut down after each diagnostic.

Linux's [device table](https://github.com/torvalds/linux/blob/v6.18/drivers/net/wireless/realtek/rtw88/rtw8821au.c)
selects the RTL8821A implementation for this adapter. It found the matching
firmware file and parsed version 42.4.0, but this is not evidence of firmware
execution on the radio. The actual failure was:

```text
failed to poll offset=0x5 mask=0x1 value=0x0
mac power on failed
probe with driver rtw88_8821au failed with error -16
```

The post-move trace starts with six reads of `0x05=0x00`. After the driver's
MAC-enable write, all 19,877 captured polls returned `0x01`. The capped trace
contains 19,905 completed vendor-control transfers with no failed or short
completion; one final submitted request is cut off by the explicit 40,000-line
capture limit. This demonstrates a new failure from a clear request bit, not
merely an old Swift request left pending. QEMU separately emitted
`libusb_kernel_driver_active: -5 [NOT_FOUND]` warnings; successful register
exchanges do not justify describing the entire USB stack as error-free.

The owner-moved port still enumerated under the same physical hub. A test on a
non-virtualized supported OS through an independent USB/power path would be a
stronger remaining discriminator. Repeating equivalent register writes on this
host is not a supported repair. The adapter remains unqualified for OTA.

The first guest run exposed a console-capture issue: asynchronous kernel output
interleaved the encoded trace. The original log was retained, and its trace was
recovered with strict base64, length and gzip CRC checks. Subsequent runs disable
console printk during export and save completion metadata even if decoding
fails. This instrumentation issue did not change the observed driver failure.

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
- `.local/usb-wifi-driver/linux-test/`: signed archive, isolated keyring,
  verified boot manifests, bounded guest/host scripts and three captured runs.
- `.local/usb-wifi-driver/linux-test/runs/20261116T121845Z/`: the post-move,
  Linux-first run, console, USB trace and unchanged-network evidence.
- `.local/usb-wifi-driver/production-after-linux-diagnostic.json`: three fresh
  pinned reads of healthy RC16 with advancing slot-2 replay after these tests.

No Wi-Fi password, production signing key, WiZ home key or administrator token
belongs in these source artifacts or in this report.
