# Display and persistent clips

The attached ST7735R is a 128 × 160 SPI panel. The console's **Display studio**
converts images, messages, and short silent videos locally into a 16-color clip.
Preview the converted result, check its size, then choose **Save and play on device**.
The device verifies the complete clip before replacing its saved content. Playback
continues after the browser closes and resumes after restart. Play, pause, blank,
panel rotation, inversion, and panel variant are saved only on explicit changes.
Automatic schedules and additional scene logic are deferred.

The default built-in replay uses landscape orientation, a 40-pixel caption area,
10-pixel lettering and a one-pixel yellow divider. Its three messages change
every **two seconds**, independently of playback. The complete game frame is
fitted below the text with its original proportions. Built-in content resides
inside the signed application and does not consume the custom clip allowance.
See [replay controls, compression and reproduction](replay.md).

## Wiring and panel verification

| Display | ESP32-C3 |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SCK / SCL | GPIO2 |
| SDA / MOSI | GPIO4 |
| RES / RESET | GPIO0 |
| DC / A0 | GPIO1 |
| CS | GPIO5 |
| BL | 3V3 |

SDA is SPI MOSI, not I²C. The supplied Arduino library's initialization values
are adapted for the native ESP-IDF SPI driver; no Arduino runtime is used.
The separate microSD pins are not wired or used. BL is permanently powered, so
**Blank screen** renders black pixels without switching off the backlight.

A board can be provisioned before the display is connected. After attaching the
panel with this same mapping, reset or power-cycle the ESP32 so the LCD receives
its initialization sequence. USB health and advancing replay counters without a
panel verify software operation only; they cannot verify wiring or visible pixels.

Open **Panel setup & test card**. The attached panel uses the black-tab/RGB profile;
the initial red-tab/BGR setting visibly swapped orange and blue. Verify the white
perimeter, yellow upper-left and magenta upper-right corners, and red/green/blue
bars from left to right. Green-tab changes the address offsets; black-tab changes
RGB/BGR order. Rotation and inversion are independently adjustable. SPI is
write-only with this wiring: “SPI ready” reports the driver, not visual proof that
the panel is connected, correctly powered, or displaying the intended colors.

GPIO2 is a strapping pin sampled during reset. Keep external circuitry from
driving it low at boot; the display's SCK input should be high impedance. The
[Espressif datasheet](https://documentation.espressif.com/esp32-c3_datasheet_en.html)
defines the boot strap requirements. The
[supplied Arduino discussion](https://forum.arduino.cc/t/cheap-1-8-tft-st7735r-how-to-wire/186097)
helps identify the alternate labels; its Arduino voltage/pin examples are not
instructions to apply 5 V to ESP32-C3 GPIOs.

## Storage and quality

All 4 MB are already allocated to the original OTA/recovery partition map.
Clips therefore have a hard **12,288-byte limit** in the existing 32 KiB `state`
NVS partition, under a separate `media_v1` namespace. This limit leaves room for
the old and new blob generations, the OTA journal, configuration, and NVS garbage
collection. Neither application slot nor crash storage is reduced. Do not add
unbounded records to this partition without re-evaluating that reservation.

One full-resolution still takes about 10 KiB. Video duration depends on motion
and detail: simple animation compresses much better than camera footage. The
editor supports 128×160, 64×80, 32×40, or 16×20, up to 12 frames/second, 120 frames,
and 60 seconds, with a shared adaptive 16-color RGB565 palette. All limits must
hold simultaneously. Oversized drafts cannot be saved; shorten them or reduce
resolution/frame rate. Source video must be decodable by the current browser.
There is no MP4 decoder on the ESP32 and no audio playback. Actual frame timing
is best effort; diagnostics report rendered and late frames.

NVS replaces a blob transactionally. The old clip remains in RAM if a save fails;
after power loss, NVS recovery selects a complete value and the application
revalidates its bounds and CRC. It never erases storage automatically on error.
The [ESP-IDF NVS documentation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/storage/nvs_flash.html)
describes the power-loss guarantees and their electrical limitations. Network
transfers are fully received and checked before any media flash write. Identical
retries do not rewrite flash. Browser/CLI resolve a lost save reply by hash
readback, not by blindly repeating a replacement.

Custom playback uses a fixed 10,240-byte packed frame, a 12,288-byte clip buffer,
and a 1,280-byte DMA strip. The clip buffers share a union with the bounded replay
decoder. A task one priority above IDLE updates the panel at 8 MHz and sleeps at
least 10 ms between bounded frame operations. Network/health tasks can preempt it.
Playback freezes during OTA writes. Media mutations use the same
operation lock as firmware/network changes and are rejected while OTA is busy.
No clip data is executed as code. Older OTA-only firmware ignores this new
namespace and can still read its original credentials and update journal.

## CLI

```sh
./entrance display --host DEVICE_IP
./entrance media info --file display.eclip
./entrance media upload --host DEVICE_IP --file display.eclip --play
./entrance display --host DEVICE_IP --mode pause
./entrance display --host DEVICE_IP --mode test --profile red --rotation 0
./entrance media download --host DEVICE_IP --file backup.eclip
./entrance media delete --host DEVICE_IP
```

Use **Export clip** or **Load saved clip** in the browser to back up content.
The normal full-flash USB backup also includes it. Production signing keys are
never used for user media; the authenticated administrator controls content.

## ECL1 wire format

Little-endian integers; maximum 12,288 bytes. Bytes 0–3 are `ECL1`, 4–7 total
length, 8–11 IEEE CRC32 over bytes 12 onward. Bytes 12/13 are width/height,
14 frame rate, 15 zero, 16–17 frame count, 18–19 zero. Bytes 20–47 hold a
zero-padded printable ASCII label (27 characters maximum). Bytes 48–79 are
16 RGB565 palette entries. Rasterized message content may contain Unicode.

Each frame is a uint16 payload length including a one-byte mode. Mode 0 is
packed 4-bit indices, first pixel in the high nibble, with a zero unused low
nibble for odd pixel counts. The first frame must be mode 0. Mode 1 applies
delta runs to the previous frame: low 7 bits plus one give the run length;
bit 7 clear skips unchanged pixels, bit 7 set consumes one color-index byte
(0–15) and fills the run. Runs must cover exactly width×height pixels. No
trailing bytes, unknown flags, zero-sized frames, or out-of-bounds runs are
accepted. At loop start the first raw frame restores a deterministic image.
