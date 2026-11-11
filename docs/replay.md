> Historical Mario implementation. The current built-in animation is [Neko wave](neko.md); the former asset and capture fixtures are retained for reproducibility.

# Built-in scene replay

The device plays a recorded sequence from the original NES Super Mario Bros.
using ordinary scripted controller inputs. No imported speedrun or TAS movie
is used. The script is planned offline, then replayed continuously from power-on
without state edits or rewinds in the recording. Movement favors walking, with
running when a jump requires it. The ESP32 does not emulate the NES CPU, execute ROM program code, or
accept game controls. It reconstructs recorded backgrounds and sprites.

Three independent messages rotate every two seconds:

1. Shenanigans by the 203 dudes
2. Achyuth & Jaideep
3. Knock the mario theme to unlock the door

The landscape display gives captions 40 of its 128 rows, followed by a yellow
divider. Nearest-neighbor scaling fits the complete 256×240 game image below it.
RGB565 output and the small panel reduce color and spatial precision. There is
no sound or door-unlock behavior. The first and last eight held scenes fade to
black so the game loop joins cleanly while the caption clock continues.

## Controls and storage

The console can play, pause, restart, jump to a level and load a local preview.
Playback continues after the browser closes. The preview identifies its own
timeline rather than claiming to be a live camera view of the panel.

```sh
./entrance display --host DEVICE_IP --mode replay
./entrance display --host DEVICE_IP --mode replay_pause
./entrance display --host DEVICE_IP --mode replay --replay-chapter 0
python3 tools/replay.py assets/entrance.rpl
```

The asset is part of the RSA-signed application. Its maximum size is 737,280
bytes; the build separately enforces the complete signed slot limit. A shared
8 KiB tile set, recorded nametables, sprite positions, palettes and scroll spans
replace full video frames. Modular byte differences are compressed in independent
zlib blocks, each expanding to at most 16 KiB. Frames are bounded to 2,432 bytes,
blocks to 128 frames and each scene to eight scroll spans. The decoder validates
indices, lengths, ordering, CRC, scene fields and every delta before use.

The replay decoder and custom clip buffers share storage. Changing mode validates
the destination content before committing settings. `config_v2` retains the old
`config` record so older firmware can still read its display settings on rollback.
Replay playback never writes flash; only explicit configuration/media changes do.

## Reproduce the asset

Use the exact original-game ROM revision identified by SHA-256
`f61548fdf1670cffefcc4f0b7bdcdd9eaba0c226e3b74f8666071496988248de`.
Keep the ROM under ignored `.local/`; it is not a source-repository dependency.
The source supplied by the project owner was the
[Japan/USA revision in vmartinv/nek](https://github.com/vmartinv/nek/blob/master/roms/Super%20Mario%20Bros.%20%28Japan%2C%20USA%29.nes).

The offline emulator is [FCEUmm](https://github.com/libretro/libretro-fceumm), pinned
to `e0f3086a8104f8fbbe49a2615b10401504a9533f`. The small patch exports read-only PPU
snapshots; it does not change the game, inputs, CPU, collision behavior or RAM.

```sh
git clone https://github.com/libretro/libretro-fceumm .tools/fceumm
git -C .tools/fceumm checkout e0f3086a8104f8fbbe49a2615b10401504a9533f
git -C .tools/fceumm apply ../../tools/fceumm-capture.patch
make -C .tools/fceumm -j4 platform=osx
python3 tools/capture_replay.py \
  --rom .local/source.nes \
  --core .tools/fceumm/fceumm_libretro.dylib \
  --controls assets/replay-controls.json \
  --output assets/entrance.rpl \
  --report assets/replay-provenance.json --sample 6
```

On Linux, use `platform=unix` and `fceumm_libretro.so`. A C compiler, make and
Python 3.12 are sufficient. `--sample` accepts 6, 8, 10 or 12 native frames; lower
sampling rates can fit a longer recording without increasing firmware bounds.
The command refuses a mismatched ROM or emulator revision and will not replace
the asset if compression exceeds the limit or a sampled pixel differs.

The recorder drives the full controller sequence from power-on and compares
every sampled 256×240 palette-index frame against the **shipped C renderer**.
Only after zero mismatches does it pack the asset. Provenance records source
hashes, sample count, timing, compressed size and the final game mode. This proves
sampled raster equivalence for that recording; it does not make the ESP a general
NES emulator or establish physical LCD color calibration.

Game graphics remain Nintendo's copyrighted material and are outside the
repository's code license. ROM program bytes are excluded. FCEUmm is GPL-2.0
software used only for offline capture, and is not linked into device firmware.
