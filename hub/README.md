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
