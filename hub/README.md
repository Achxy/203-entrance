# USB-to-Wi-Fi maintenance hub

A spare ESP32-C3 gives the Mac a separate Wi-Fi radio through its native USB
serial port. The Mac keeps its Internet connection. No network interface,
default route, Wi-Fi configuration, DNS setting or system proxy is changed.

The C3 has a [fixed-function USB Serial/JTAG controller](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-guides/usb-serial-jtag-console.html),
so this project uses a USB byte tunnel and a loopback-only TCP proxy. The usual
administrator CLI performs TLS and certificate pinning directly with the
production device. The hub cannot decrypt the administrator traffic and has
no production administrator password or signing key. Its Wi-Fi credentials
are supplied over USB and retained only in RAM. It opens no Wi-Fi listener.

The hub is a separate firmware project, built with the same pinned ESP-IDF.
It deliberately has no OTA service. USB remains its programming/recovery path;
no eFuses are changed. The original Super Mini's validated 8.5 dBm transmit
power cap is retained. The installed display is not a hub-flashing target.

## Setup and operation

Run commands from the repository root after the standard `./entrance setup`.

```sh
./hubctl build
./hubctl flash --port /dev/cu.usbmodem101 --expected-mac SPARE_MAC
```

Flashing verifies the chip, MAC and security state, takes a full backup, and
refuses the board selected by `.local/device.json`. An existing verified full
backup can be supplied with `--backup DIRECTORY`. Erasing the spare removes
its old OTA identity/settings; its original flash remains recoverable from
that private backup.

Create a private, ignored JSON file containing `ssid` and `password`. For the
display's guest hotspot use its documented guest credentials. Then start:

```sh
./hubctl serve --port /dev/cu.usbmodem101 --expected-mac SPARE_MAC \
  --wifi-profile .local/hub/guest-wifi.json --status-file .local/hub/status.json
```

The listener binds only `127.0.0.1:18443`, forwarding through the spare to
`192.168.4.1:443`. One TCP connection is admitted at a time; excess local
connections close immediately. Keep the proxy running in its terminal.

```sh
./entrance status --host 127.0.0.1:18443 --tls12 \
  --device-profile .local/devices/PRODUCTION/device.json
./entrance upload dist/v0.3.0-rc.12/firmware.bin --host 127.0.0.1:18443 --tls12 \
  --device-profile .local/devices/PRODUCTION/device.json
```

These commands retain the existing production certificate pin and signed-image
checks. `--tls12` selects a smaller handshake for the device’s TLS-1.2-only
server; it does not disable CA or fingerprint validation. USB ownership is exclusive; stop the proxy with Ctrl-C before scanning
or flashing the hub. A scan deliberately disconnects only the hub's station:

```sh
./hubctl scan --port /dev/cu.usbmodem101 --expected-mac SPARE_MAC
```

The default scan filters for `203 IoTitty`. `--ssid NAME` selects a different
network. `--target` and `--target-port` select a different numeric IPv4 TCP
destination for an explicitly configured hub connection. They never change
Mac routes. The default listener is a CLI maintenance path, not a browser
certificate workaround or a public administrator gateway.

## Bounds and failures

Frames use a 16-byte versioned header, CRC-32 covering header and payload, a
per-connection random ID, and payloads capped at 2,048 bytes. One acknowledged
chunk per direction applies backpressure; queues do not grow with traffic.
Corrupt frames, stale connection IDs, socket errors, USB loss and acknowledgement
timeouts close the affected stream. TLS and the OTA engine still enforce their
own integrity/offset rules. A new client opens a fresh stream after a display
reboot; the existing OTA CLI reconnects and confirms the trial automatically.

Each local connection also owns its own acknowledgement event and client
identity. Reader callbacks and cleanup retain that ownership: a delayed CLOSE,
DATA failure or ACK from an old stream cannot detach or acknowledge a replacement
stream. Deterministic thread tests exercise these races without physical USB.
This host-side fix does not repair radio disassociation or establish OTA
reliability on a weak Wi-Fi link.

The hub retries Wi-Fi association without changing the laptop's connection.
Three consecutive failed TCP opens also trigger a fresh association of only
the hub's radio, at most once per minute. This recovers stale associations
across display reboots/channel changes. An open stream is never interrupted
by this recovery, and application requests are never replayed by the hub.
The supervisor polls bounded status every two seconds. The status file contains
the hub MAC, IP, signal, uptime, heap, byte counters and corrupt-frame count;
it contains no Wi-Fi or administrator password. Initial association has a
45-second limit. If it fails, inspect the reason and run a targeted scan.

Native/Python codec tests cover every payload size, fragmented streams, boot
noise and corruption. Socket-pair tests cover bidirectional data, reconnects
and stale-session isolation. Those tests do not establish physical radio
reachability; record real scans, pinned HTTPS and OTA results separately.

## Read-only WiZ diagnostics (hub 1.1.0)

Stop the TCP proxy before this command. It verifies the spare's expected MAC and
firmware before joining the Wi-Fi profile. The laptop's networking is untouched.
Credentials are accepted only through the JSON profile file, never command-line
SSID/password arguments.

    ./hubctl wiz-query --port /dev/cu.usbmodem101 --expected-mac SPARE_MAC \
      --wifi-profile .local/lights/oneplus-wifi.json \
      --home-export /path/to/private-wiz-home-export.json \
      --output .local/lights/diagnostics/wiz-read.json

The owner-authorized export must have version "1.0" and one to eight
devices[].mac_address entries. Only those MACs are retained from the export;
its signing key, home IDs, traits and credentials are not used, transmitted or
copied into the diagnostic. Keep the profile, export and report out of source
control. The output directory is created with mode 0700; an existing nonprivate
directory is rejected without changing its permissions. The report is atomically
written mode 0600. Console output contains counts and bounded model/firmware/CCT
information, never full MACs, IPs, home IDs, credentials or raw user configuration.

The diagnostic sends at most two subnet-broadcast getSystemConfig requests.
Only matching replies from exported MACs become targets. It then asks those
identified endpoints for getModelConfig, getUserConfig, getPilot, and optional
getTime, with at most two attempts per method. There is no arbitrary-method,
arbitrary-packet, setPilot, registration, reset, pairing, or signed-write path.
The first four method shapes follow the pinned
[pywizlight source](https://github.com/sbidy/pywizlight/blob/19e607afe0eb6db1473602639e194d691234d4fd/pywizlight/bulb.py).
Optional-method rejection and timeouts are legitimate outcomes. A locked bulb
may require authentication even for reads; these diagnostics use no home key.

Packets bind the hub's station interface and local IPv4 address, stay within
its RFC1918 subnet, and use UDP 38899. Public, multicast, network, broadcast
response addresses and the hub's own address are rejected. Discovery is 1.6 s;
each targeted read is bounded to 700 ms and the entire operation to 25 s.
At most four receive packets are processed per main-loop pass. The maximum
operation is 66 small outgoing reads and 40 accepted replies. Conflicting MAC/IP
claims are excluded from targeted reads. These checks are correlation, **not
cryptographic authentication** of a bulb.

USB framing stays at protocol 1 and a 2048-byte payload limit. Command 8 carries
version 1, count and raw allowed MACs. A start reply precedes paired metadata
(0x89, 16 bytes) and raw reply-body (0x8a, at most 2048 bytes) frames, then a
bounded summary (0x8b). Every frame retains the request ID and CRC. The extra
2049th UDP receive byte detects oversized datagrams; truncated prefixes are never
parsed. Duplicate decoded JSON keys, excessive nesting/token budgets, unmatched
method/MAC/IP and unexpected stream ordering are rejected. TCP forwarding and
Wi-Fi changes are rejected while a diagnostic is active; its Wi-Fi configuration
is not restored automatically afterward. Run the TCP proxy with the desired
profile again when maintenance is complete.

Run the eleven check groups with:

    python3 -m unittest discover -s tests -p test_usb_hub.py -v

They cover existing TCP forwarding/reconnect/isolation, all 2049 USB payload
sizes, metadata/body matching, privacy/permissions, maximum-size/oversized UDP,
all fixed read methods, retries/deadlines, identity conflicts and 2000 arbitrary
datagrams in the C parser under ASan/UBSan. The pinned ESP-IDF ESP32-C3
compile/link passed. These checks do not claim a successful physical light read,
Wi-Fi association or control.

The console recognizes cctRange on newer models, extRange, and legacy whiteRange,
preserving the reported array and naming its source field. A sanitized physical
ESP25_SHRGB_01 / 1.38.0 model response is included as a regression fixture; its
2200–6500 K range and minimum dimming 10 appear in the summary. Gradient and
white-channel flags are not presented as implemented control capabilities.
