# USB-to-Wi-Fi hub checkpoint — 12 November 2026

The owner explicitly repurposed the USB-attached original ESP32-C3 as a hub.
Its factory MAC is distinct from the installed display. The flashing tool
requires the expected spare MAC and refuses the active production profile.
Exact device identifiers and campus addresses remain in private evidence. No Mac Wi-Fi, route, DNS or system-proxy setting changed.

## Latest checkpoint: maintenance hub released

RC14 subsequently installed and confirmed healthy through this hub at
31.7 seconds. It renders the owner's requested white `203` and retains the
yellow divider, Neko loop and game code. Its automatic trial uplink delay
allowed reconnection without manually pausing or rewriting campus settings.
Authenticated post-installation samples verified advancing animation,
healthy settings, a valid RC12 rollback target and an online Internet relay.

The hub process was then stopped. The public relay still reported the
production display online through its independent campus connection. The
spare can be unplugged; no laptop or hub is required for autonomous display
operation. The owner deferred longer qualification for the day. This is not
a 60-minute soak or a complete Internet game acceptance result. The older
checkpoints below retain the unsuccessful attempts and their scope.

## Preservation and installation

A fresh, complete 4,194,304-byte flash backup was read before erasure and its
SHA-256 verified. Private location: `.local/backups/20261112T081830Z/`.
The original signed OTA identity/settings can be recovered from that image.
The spare was erased and programmed with the separate hub bootloader,
partition table and application. The write hashes verified. No eFuse write
was performed. Production signing keys and device profiles remain intact.

The spare's earlier hardware validation required a transmit-power cap of
8.5 dBm. The first hub build omitted it and did not complete association
(reason 2). The corrected hub retains that limit. Its final image SHA-256 is
`2a60630a55520f25db9aef5057f22d50f3f1b3ad947bae9572e16f40890169c2`.

## Observed bridge behavior

- A real USB scan found `203 IoTitty` on channel 6 with the expected AP MAC
  recorded in the private production profile. Signal was approximately −85 dBm.
- The spare associated and received `192.168.4.2`. The host listener binds
  only `127.0.0.1:18443`, forwarding to the display's `192.168.4.1:443`.
- The first default TLS handshake ended without a response. An explicit
  smaller TLS 1.2 handshake completed in about 2.14 seconds, verified the
  production certificate pin, and received the expected HTTP 401 without
  credentials. This does not prove why the first handshake failed.
- Authenticated status then succeeded through the same bridge. The installed
  display reported confirmed, healthy `0.3.0-rc.9`, healthy settings, and
  association to its saved campus network, RSSI −55 dBm. Its prior recorded
  DHCP address was stale. MAC addresses remain factory-derived.
- Relay diagnostics still reported zero successful connections, 71
  disconnections and an ESP-TLS handshake error. Campus association is not an
  Internet-relay pass. No production update has been performed at this checkpoint.
- A later management reconnect failed because the hub could no longer find
  the display hotspot (reason 201). The earlier weak signal and subsequent
  loss mean that a continuous radio link is **not qualified**. The supervisor
  remains able to answer USB status and retries association. At the same time,
  the Mac received HTTP 200 from the public site using its existing interface.

The `--tls12` CLI option is explicit, leaves the default mode unchanged and
retains CA validation and the independent certificate fingerprint check. The
current MCU firmware enables TLS 1.2 and disables TLS 1.3. The hub forwards
opaque bytes; it does not terminate TLS or receive administrator credentials.

## Automated checks and limits

All 74 host tests pass. New native/Python framing checks cover every payload
size from 0 through 2,048 bytes, CRC agreement, fragmentation, boot noise,
corruption and size bounds. Socket-pair tests cover byte-for-byte data,
reconnects and stale-session isolation. TLS tests verify that a wrong pin
blocks an administrator request in both default and explicit TLS 1.2 modes.
The hub compiles using the pinned ESP-IDF with a bounded 8 KiB main stack.
All four [CI jobs for the implementation](https://github.com/Achxy/203-entrance/actions/runs/43190588980)
passed, including the separate hub firmware build.

These observations establish a working USB/radio/pinned-HTTPS management
route. They do not yet establish an OTA transfer through the hub, Internet
gameplay, physical power-loss behavior or the final 60-minute operating soak.
The signed RC12 candidate remains available for the separately approved
production installation. The proxy must remain running for OTA confirmation
and monitoring; USB loss fails the connection rather than changing Mac Wi-Fi.


## RC12 trial attempts through the hub

After explicit owner approval on 12 November, two complete RC12 uploads
transferred all 1,314,816 bytes and passed image verification. Both selected
the inactive application and rebooted, but neither received updater contact
during its trial. Authenticated/ticket-scoped reads subsequently confirmed
that the device restored healthy RC9. RC12 is marked invalid; the preserved
running RC9 slot is valid. These are safe rollback observations, **not a
successful RC12 installation**.

The USB tunnel reported zero corrupt frames. Initially its radio signal was
approximately −91 dBm. Moving the spare improved it to about −71 dBm, and
three authenticated status requests completed with advancing animation.
The following upload still lost hotspot discovery at reboot (reason 201).
Signal after recovery was again weak. This does not establish whether the
cause is channel behavior, trial maintenance state, interference or another
firmware issue. Further isolation is required before another update.

The host supervisor now refreshes only the spare's association after three
consecutive failed TCP opens, at most once per minute. It never interrupts an
open stream or replays application data. Recovery rejoined after the failed
trial, but did not rescue that trial. All 75 host tests pass, including bounds
and error classification for this recovery. Laptop network settings remain
unchanged. Campus Internet gameplay and the final 60-minute soak remain open.


### Successful RC12 installation

With explicit approval, the display's open-network uplink was temporarily
paused for one further trial. The signed RC12 image transferred, rebooted,
received fresh confirmation tickets and confirmed healthy at 33.2 seconds.
The script restored the saved campus configuration immediately afterward;
subsequent status reported association, valid time and healthy settings.
The fixed red `203` caption and advancing Neko frame/loop counters are reported
by RC12. RC9 remains a valid rollback target. The actual station/AP addresses
match the original fixed factory identities in private records.

This isolates uplink activity during trial startup as a material factor in the
failed reconnects. It does not establish a particular RF or channel-switch
mechanism. Follow-up firmware gives hotspot confirmation a 45-second head start,
then resumes the station even if an updater reachable only over the uplink
still needs to confirm. Earlier confirmation resumes the station after an
observation grace of up to 10 seconds, without rewriting settings.
The uplink restoration, signature checks and healthy trial all used the USB
hub; the laptop's network settings remained unchanged.
