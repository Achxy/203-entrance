# Neko wave

The default display is a seated pixel-art beckoning cat derived from the owner's
reference. Only its raised paw moves. The animation repeats every **2.4 seconds**:
24 frames at 10 fps, eight distinct poses, 96 × 87 pixels, and a shared 16-color
RGB565 palette. The complete ECL1 asset is **10,582 bytes**, including its header,
palette and checksum. It is embedded in the signed application; custom media
retains its separate 12 KiB allowance.

The landscape display retains the exact 40-row header area and one-row yellow
divider at row 40. A fixed **203** replaces the former three rotating messages.
The original bitmap font is scaled 4× without smoothing: the white RGB565 `0xFFFF`
ink occupies x=36–123 and y=6–33, centered horizontally and vertically. The Neko
pixels and scaling below the divider are unchanged. This label does not
implement or change any unlocking or game behavior.

## Motion and pixel review

The motion is an inward/downward beckon with a fixed elbow attachment, followed
by its reverse. The body, face, ears, collar, coin and feet use the same master
raster in every frame. Endpoint holds ease the reversal; the last and first
frames match exactly. There is no fade to black at the wrap.

Research informing the choice:

- The [Japan Tourism Agency's Tokoname account](https://www.mlit.go.jp/kankocho/content/001736018.pdf)
  describes the raised hand and up/down beckoning gesture. Its description of
  the left paw welcoming people matches the reference's viewer-right paw.
- [JNTO's account](https://faq.japan-travel.jnto.go.jp/en/japan-magazine/a-trail-of-cats-journey-through-culture-folklore-and-hidden-dest/)
  corroborates the palm-down beckon. The sprite is a stylized interpretation,
  not a motion-capture reconstruction.
- [Aseprite's animation workflow](https://aseprite.org/docs/animation/) and
  [sprite-sheet documentation](https://aseprite.org/docs/sprite-sheet/) informed
  explicit frame reuse, timing and sheet inspection.

All 24 display-sized frames and all eight poses were visually inspected together
for body drift, paw attachment, edge artifacts, colors and cropping. The initial
sheet's body variations were excluded; faint background residue was removed.
An adaptive palette initially dulled the red collar, so the final conversion
uses an explicit palette preserving red, pink, gold, orange, olive and yellow.
The reviewed contact sheet and GIF/MP4 previews are reproducible artifacts under
`output/neko/`, which is not checked into source control.

Automated checks execute the production C decoder through 100 complete loops,
compare all frame hashes with the packed artwork, and compare **144 complete
LCD rasters** between C and browser: every frame × both orientations × all three
captions. Corruption, truncation, invalid seek and loop restart are checked.
The decoder keeps a fixed 10,240-byte packed frame and never allocates per frame.
This is software/pixel evidence. The SPI panel provides no pixel readback;
physical color/visibility confirmation and operating soak remain separate checks.

## Reproduce and edit

The generation tool and exact prompt are in
[art direction](../assets/neko/art-direction.md). The original generated sheet,
final asset, fixed palette, frame order and hashes are kept in
[`assets/neko/`](../assets/neko/). Image generation is not needed to reproduce the
packed asset or previews.

With Node 22–24, ImageMagick and FFmpeg installed:

```sh
node tools/build_neko.mjs
node tests/neko.mjs
./entrance test
```

Outputs include `neko-wave.gif` (infinite loop), `neko-wave.mp4`, individual PNG
frames, `all-frames.png`, and `frame-audit.json`. The firmware and browser consume
identical ECL1 bytes. The browser's Neko controls support play, pause, restart and
local preview. Custom images/videos remain available through Display studio.

The `/api/v1/display/replay` path and `replay`/`replay_pause` mode names are kept
for API compatibility. The returned asset is now ECL1; status reports its
`format`, `title`, dimensions and timing. `replay_chapter: 0` restarts the loop;
other chapter indices are rejected. Former game capture tools and fixtures are
retained for historical reproducibility but are not linked or embedded in the
current firmware.
