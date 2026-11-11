# Recovery and backups

## Keep these private artifacts

Before replacing firmware, `entrance flash` makes a full 4 MB backup with its SHA-256 and hardware/security information in `.local/backups/<UTC timestamp>/`. The first inspected flash is also retained locally as `.local/original-flash.bin`. Backups contain credentials or application data: do not commit or upload them.

For an already provisioned board, `entrance usb-install IMAGE --port PORT` is the
settings-preserving USB update path. It requires a newer project-signed image,
verifies the current signed image and partition map, then writes only the inactive
application and alternate boot-metadata sector. It retains a full backup and
`installation.json` with image hashes, addresses and healthy-boot samples under
`.local/backups/<timestamp>-usb-install/`. Native USB may briefly re-enumerate.

The physical installer verifies 30 continuous healthy seconds and rejects a
restart, stale telemetry, unexpected version or unhealthy services. It installs
a trusted USB baseline rather than starting an OTA ticket trial. If that health
check fails, the command reports failure even though the candidate may already
be installed. Retain the backup and use explicit rollback or the full-flash USB
restore below. A validation/transfer error attempts to release the ROM downloader;
press RESET or reconnect if the cable disappeared during that recovery attempt.

Keep an encrypted offline copy of `.local/keys/`, `.local/device.json`, `.local/devices/`, the original flash backup and the matching metadata. The profile contains the hotspot and administrator passwords and certificate pin. Copy the whole set together because the profile references its certificate and key files. Preserve the original files when moving paths; edit the private profile paths to their restored locations.

A release application alone cannot recreate device credentials. Recovery artifacts and signing keys are deliberately excluded from the public repository and releases.

## Local management and rollback

Join the device hotspot and use `https://192.168.4.1` or `./entrance status`. An unavailable Internet connection never prevents this. `./entrance rollback` restores a previous valid image if one exists. Rollback is unavailable after the first installation until at least one successful OTA update has established a second working image. It is also unavailable if the previous image has been invalidated or overwritten by an interrupted transfer.

A trial must receive its updater ticket and pass 30 continuous healthy seconds before 120 seconds from boot. Keep the browser tab or CLI running. An interrupted trial, crash or reset returns to the previous valid image on boot. If no valid previous image exists, use USB recovery.

Hold **BOOT for five seconds while running** to cancel an incomplete transfer and disable the optional station uplink until reboot. The hotspot, administrator password and certificate remain in place. Maintenance is not an authentication bypass or a factory reset. Configuration errors must be diagnosed or restored explicitly, never erased silently.

## BOOT/RESET USB recovery

1. Connect the board directly with a data-capable USB cable.
2. Hold BOOT, press and release RESET, then release BOOT. This selects the ROM downloader. If RESET is unavailable, hold BOOT while plugging USB in.
3. Run `./entrance doctor` and identify the Espressif port. On macOS it normally resembles `/dev/cu.usbmodem1101`.
4. To reinstall the signed baseline and the existing saved identity, run `./entrance flash --port PORT`. It backs up the current flash first, verifies signatures and board geometry, then replaces the bootloader, partition table and applications over USB. It does not burn eFuses.
5. Wait at least 35 seconds, join the hotspot, and check `./entrance status`.

To restore the exact original flash, verify its hash against the private metadata, enter ROM download mode and run the managed Python:

```sh
.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python -m esptool \
  --chip esp32c3 --port PORT write-flash 0x0 .local/original-flash.bin
```

This restores the original firmware and partition contents, including any original secrets. It is independent of the new OTA layout. Do not restore a backup from another device.

## Identity repair and replacement

`./entrance provision --port PORT` rewrites only the identity partition using the existing private profile. Use this to repair damaged identity storage. `--reset-access` explicitly creates new administrator/hotspot credentials and a new HTTPS identity; the previous profile and certificate files are backed up under `.local/access-backups/`. Re-enroll the replacement certificate after checking its fingerprint over the trusted USB provisioning path.

For a signing-key replacement, keep the old recovery artifacts, generate the replacement key in a separate checkout, build and verify a new signed baseline, then reflash and provision over USB. There is no OTA key-rotation endpoint. Losing the production private signing key prevents signing updates accepted by the existing firmware, but does not disable USB recovery.

## Crash capture

A 128 KiB `coredump` partition retains ESP-IDF ELF crash data. Save a full flash backup before reinstalling, together with the exact build ELF and firmware version. With the matching SDK environment, use `idf.py coredump-info` / `idf.py coredump-debug` over USB. Crash dumps can include RAM contents and credentials; keep them private. The browser diagnostic JSON is a bounded, redacted event/status export, not a RAM dump.

## Power interruption validation

A software reset is not a power interruption. Record unplug/replug or a measured switched-USB power cut separately, including its exact stage: receiving data, after verification/activation, or during pending trial. After restoring power, verify the running version, valid-slot metadata, settings and management access. Never claim that software-reset tests establish physical power-loss behavior.

For a manual activation-stage test, install a qualification image built with `--fault activation` first. Its next accepted update waits five minutes after boot selection before the scheduled reboot, allowing an explicit unplug cue before the candidate's first boot. Production builds retain the normal two-second reboot delay. Cut well inside this window and record the timing; this tests post-selection recovery, not interruption of an individual flash-write instruction. Fault builds are rejected by release publication.
