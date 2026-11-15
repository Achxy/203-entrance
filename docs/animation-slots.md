# Saved animation slots

The signed firmware contains two independent animations. Selecting one does
not replace the other, consume the custom clip allowance, alter the panel
wiring/settings, or change game code. Selection is available to the
administrator in **Display studio → Saved animation → Use this slot**.
Public game guests cannot change it.

| Slot | Content | Landscape layout |
|---|---|---|
| 1 | Original Neko wave | Original white `203`, header rows 0–39, yellow row 40, unchanged Neko pixels |
| 2 | Study paw | White `203` at 3× font scale, header rows 0–31, yellow row 32, captions rows 33–52, animation rows 53–127 |

To return to the original, select **1 · Original Neko — restore original
layout** and press **Use this slot**. It starts playing and survives reboot.
The CLI uses the same authenticated API:

```sh
./entrance display --animation-slot 1 --mode replay --host DEVICE_ADDRESS
./entrance display --animation-slot 2 --mode replay --host DEVICE_ADDRESS
```

Through the spare USB hub, use `--host 127.0.0.1:18443 --tls12` and the private
production profile as described in [the hub instructions](../hub/README.md).
The hub never changes laptop networking. It is needed only for local
maintenance, not for autonomous animation or campus relay operation.

## Original restore point

Before the change, authenticated readback verified healthy RC14 and matched
its Neko asset byte-for-byte. The private restore bundle is at
`.local/restore-points/20261116-neko-rc14/`. `complete.json` lists SHA-256 values
for the original signed release, source archive, exact animation, readable
configuration and saved USB identity. Keep this directory private: it contains
administrator and recovery credentials. It is not a new full-flash dump.

The easiest restoration is slot 1 in the new firmware. The RC14 release and
source remain preserved separately. Normal OTA deliberately refuses a
version downgrade; restoring the entire old firmware requires a valid previous
slot, USB recovery, or rebuilding the archived source as a newer signed
version. Do not disable the version/signature checks.

## Study animation

The owner's original photograph supplies the face and the foreground paw.
Deterministic pixel conversion preserves the original expression and proportions;
a generated chest patch is used only behind the removed foreground paw.
Rejected character redesigns are not included. The body remains fixed and the
paw reaches toward the viewer and withdraws with a smooth periodic transform.
Its resting position is eight landscape LCD pixels to the viewer's right of center.
The first and final frames match exactly.

The loop has 36 frames at 12 fps, 96 × 75 pixels, and a shared 16-color RGB565
palette. Its ECL1 asset is 30,694 bytes. Built-in assets have a separate bounded
64 KiB validation limit. The persistent custom clip limit remains 12 KiB;
the decoder still uses a fixed 10,240-byte packed pixel buffer and allocates
nothing per frame.

These exact captions cycle independently every **1,350 ms**:

```text
*paws at you* please study for endsems vro
*paws at you* actually vro
*paws at you* さよなら vro
```

Both asterisks are ordinary visible glyphs. No angle brackets, quotation marks,
or Markdown formatting are drawn. Lines are centered and wrapped to the panel;
the four Japanese glyphs come from the author's
[Misaki Gothic 8×8 bitmap font](https://littlelimit.net/misaki.htm). The subset,
source archive hash and permissive license are included under `assets/study/`.
Pausing or restarting the paw does not restart the independent caption clock.
Switching animation slots starts a fresh caption cycle. Games retain their
existing screen behavior and return to the selected idle animation afterward.

## Persistence and verification

A CRC-protected `config_v3` record atomically stores slot and panel/playback
selection. Existing `config_v2` and legacy records are left intact so RC14
rollback can read its original display settings. Migration selects slot 1.
Unknown slots, malformed records and conflicting OTA changes are rejected.

The original 144 Neko C/browser LCD rasters remain unchanged. New checks compare
all 216 study LCD rasters, all captions and both orientations. Native tests
exercise 100 complete loops, exact 1.35-second boundaries, literal Unicode text,
size-limit separation and all legacy panel configurations. The soak evaluator
also checks that the selected animation does not change during a window.
Software/raster results do not prove physical panel appearance or a 60-minute
operating soak; hardware installation evidence is recorded separately.

## Reproduce the artwork and previews

```sh
node tools/build_study.mjs
node tools/preview_study.mjs
node tests/neko.mjs
node tests/study.mjs
./entrance test
npm test
```

The preview exporter makes a full 81-second joint loop: 27 paw loops and 20
three-caption cycles. Outputs in `output/study/` include GIF/MP4 previews,
all individual frames and a contact sheet. The art tool and exact final prompt
are in [the artwork record](../assets/study/art-direction.md).
