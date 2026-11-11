"""Capture verified NES scenes using the pinned, read-only-instrumented FCEUmm core.

This is an offline development tool. Neither the emulator nor ROM program data
is linked into the ESP32 application. See docs/replay.md for reproducible setup.
"""
from __future__ import annotations
import argparse
import ctypes as C
import hashlib
import json
import math
import re
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from replay import pack, inspect

ROOT = Path(__file__).resolve().parents[1]
ROM_SHA256 = "f61548fdf1670cffefcc4f0b7bdcdd9eaba0c226e3b74f8666071496988248de"
CORE_REVISION = "e0f3086a8104f8fbbe49a2615b10401504a9533f"
FRAME_RATE = 60.0988138974405


class Core:
    """Minimal libretro host; controller input and read-only capture only."""
    def __init__(self, library: Path, rom: Path, directory: Path):
        self.lib = C.CDLL(str(library.resolve()))
        self.keys = 0
        self.directory = str(directory.resolve()).encode()
        self.callbacks = []

        class Variable(C.Structure):
            _fields_ = [('key', C.c_char_p), ('value', C.c_char_p)]

        def environment(command, pointer):
            if command == 10:
                return True  # Pixel format; capture uses the original index buffer.
            if command == 3:
                C.cast(pointer, C.POINTER(C.c_bool))[0] = False
                return True
            if command in (9, 31):
                C.cast(pointer, C.POINTER(C.c_char_p))[0] = self.directory
                return True
            if command == 15:
                variable = C.cast(pointer, C.POINTER(Variable)).contents
                options = {b'fceumm_overscan_h': b'disabled', b'fceumm_overscan_v': b'disabled',
                           b'fceumm_turbo_enable': b'None'}
                if variable.key in options:
                    variable.value = options[variable.key]
                    return True
            if command == 17:
                C.cast(pointer, C.POINTER(C.c_bool))[0] = False
                return True
            return False

        specs = [
            ('environment', C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p), environment),
            ('video_refresh', C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t), lambda *_: None),
            ('audio_sample', C.CFUNCTYPE(None, C.c_int16, C.c_int16), lambda *_: None),
            ('audio_sample_batch', C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t), lambda _, n: n),
            ('input_poll', C.CFUNCTYPE(None), lambda: None),
            ('input_state', C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint),
             lambda port, _device, _index, key: int(port == 0 and bool(self.keys & (1 << key)))),
        ]
        for name, signature, function in specs:
            callback = signature(function)
            self.callbacks.append(callback)
            getattr(self.lib, 'retro_set_' + name)(callback)
        self.lib.retro_init()

        class Game(C.Structure):
            _fields_ = [('path', C.c_char_p), ('data', C.c_void_p), ('size', C.c_size_t), ('meta', C.c_char_p)]

        self.data = C.create_string_buffer(rom.read_bytes())
        self.game = Game(str(rom.resolve()).encode(), C.cast(self.data, C.c_void_p), len(self.data) - 1, None)
        self.lib.retro_load_game.argtypes = [C.POINTER(Game)]
        self.lib.retro_load_game.restype = C.c_bool
        if not self.lib.retro_load_game(C.byref(self.game)):
            raise ValueError('The pinned emulator could not load the supplied ROM')
        self.trace = (C.c_uint16 * (240 * 4)).in_dll(self.lib, 'entrance_trace')
        self.nt = (C.c_uint8 * 2048).in_dll(self.lib, 'entrance_nt')
        self.oam = (C.c_uint8 * 256).in_dll(self.lib, 'entrance_oam')
        self.palette = (C.c_uint8 * 32).in_dll(self.lib, 'entrance_pal')
        self.pixels = C.c_void_p.in_dll(self.lib, 'XBuf')
        self.lib.retro_get_memory_data.argtypes = [C.c_uint]
        self.lib.retro_get_memory_data.restype = C.c_void_p
        self.ram = (C.c_uint8 * 2048).from_address(self.lib.retro_get_memory_data(2))

    def step(self, keys):
        self.keys = keys
        self.lib.retro_run()

    def scene(self):
        scene = bytearray(2432)
        scene[:2048], scene[2048:2304] = bytes(self.nt), bytes(self.oam)
        scene[2304:2336] = bytes(v & 63 for v in self.palette)
        spans = []
        for y in range(240):
            x, sy, control, mask = self.trace[y * 4:y * 4 + 4]
            control &= 56
            if not mask & 8:
                x = sy = 0
            if not mask & 24:
                control = 0
            previous = spans[-1] if spans else None
            if previous and (x, control, mask) == (previous[1], previous[3], previous[4]) and (
                    not mask & 8 or (previous[2] + y - previous[0]) % 240 == sy):
                continue
            spans.append((y, x, sy, control, mask))
        if len(spans) > 8:
            raise ValueError('This scene exceeds the eight-span renderer bound')
        for i, (y, x, sy, control, mask) in enumerate(spans):
            struct.pack_into('<BBHHBB', scene, 2340 + i * 8, y, 0, x, sy, control, mask)
        scene[2404:2408] = bytes((len(spans), min(7, self.ram[0x75f]), min(3, self.ram[0x75c]), self.ram[0x770]))
        return bytes(scene)


def capture(args):
    rom = args.rom.read_bytes()
    if hashlib.sha256(rom).hexdigest() != ROM_SHA256:
        raise ValueError('ROM differs from the exact revision used for these recorded controls')
    controls_bytes = args.controls.read_bytes()
    controls = json.loads(controls_bytes)
    if controls.get('format') != 1 or controls.get('rom_sha256') != ROM_SHA256:
        raise ValueError('Incompatible controller recording')
    runs = controls['runs']
    if any(len(r) != 2 or type(r[0]) is not int or not 1 <= r[0] <= 360000 or
           type(r[1]) is not int or r[1] & ~0x1e9 for r in runs):
        raise ValueError('Invalid controller run')
    total = sum(r[0] for r in runs)
    if not 240 < total <= 360000 or total != controls['frames']:
        raise ValueError('Invalid controller recording duration')
    checkout = args.core.resolve().parent
    revision = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != CORE_REVISION:
        raise ValueError('Emulator checkout differs from the pinned revision')
    patch = (ROOT / 'tools/fceumm-capture.patch').read_bytes()
    actual_patch = subprocess.check_output(['git', '-C', str(checkout), 'diff', '--', 'src/ppu.c'])
    changed = subprocess.check_output(['git', '-C', str(checkout), 'diff', '--name-only', 'HEAD'], text=True).splitlines()
    if actual_patch != patch or changed != ['src/ppu.c']:
        raise ValueError('Emulator changes differ from the reviewed read-only capture patch')
    palette_source = (checkout / 'src/palettes/palettes.h').read_text().split('pal palette[512] = {', 1)[1]
    colors = [tuple(map(int, m)) for m in re.findall(r'\{\s*(\d+),\s*(\d+),\s*(\d+)\s*\}', palette_source)[:64]]
    palette = b''.join(struct.pack('<H', ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) for r, g, b in colors)
    tiles = C.create_string_buffer(rom[32784:])
    output = C.create_string_buffer(256 * 240)
    mask = bytes(i & 63 for i in range(256))
    scenes = []
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='entrance-replay-') as temp:
        library = Path(temp) / 'scene.so'
        subprocess.run(['cc', '-O2', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'components/entrance/include'),
                        str(ROOT / 'components/entrance/replay_scene.c'),
                        str(ROOT / 'components/entrance/replay.c'),
                        str(ROOT / 'tests/native/replay_harness.c'), '-lz', '-o', str(library)], check=True)
        renderer = C.CDLL(str(library))
        renderer.replay_scene_frame.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
        core = Core(args.core, args.rom, Path(temp))
        frame = 0
        for count, keys in runs:
            for _ in range(count):
                core.step(keys)
                if frame >= 240 and frame % args.sample == 0:
                    scene = core.scene()
                    renderer.replay_scene_frame(C.create_string_buffer(scene), tiles, output)
                    actual = C.string_at(core.pixels.value, 256 * 240).translate(mask)
                    if actual != output.raw:
                        raise ValueError(f'Pixel mismatch between the emulator and shipped C renderer at source frame {frame}')
                    scenes.append(scene)
                frame += 1
                if frame % 6000 == 0:
                    print(f'{frame}/{total} input frames; {len(scenes)} exact raster matches', flush=True)
        final_mode = int(core.ram[0x770])
        core.lib.retro_unload_game()
        core.lib.retro_deinit()
    chapters = []
    for chapter in controls['chapters']:
        frame = max(0, math.ceil((chapter['start_frame'] - 240) / args.sample))
        if frame >= len(scenes):
            raise ValueError('Chapter lies outside the recorded scenes')
        chapters.append(dict(frame=frame + 8 if chapters else 0, world=chapter['world'], level=chapter['level']))
    binary = pack([scenes[0]] * 8 + scenes + [scenes[-1]] * 8, rom[32784:], palette,
                  chapters, round(args.sample / FRAME_RATE * 1000000))
    # Check the final compressed artifact through the shipped C state machine,
    # not just the scene renderer used during capture.
    renderer.open_replay.argtypes = [C.c_void_p, C.c_size_t]
    renderer.get_scene.argtypes = [C.c_void_p]
    asset = C.create_string_buffer(binary)
    decoded = C.create_string_buffer(2432)
    decoded_hash = hashlib.sha256()
    if not renderer.open_replay(asset, len(binary)):
        raise ValueError('The shipped C decoder rejected the packed replay')
    for frame, expected in enumerate([scenes[0]] * 8 + scenes + [scenes[-1]] * 8):
        renderer.get_scene(decoded)
        if renderer.current_frame() != frame or decoded.raw != expected or not renderer.advance():
            raise ValueError(f'Compressed scene round trip failed at frame {frame}')
        decoded_hash.update(decoded.raw)
    if renderer.current_frame() != 0 or renderer.loop_count() != 1:
        raise ValueError('The compressed replay did not loop correctly')
    renderer.seek_frame.argtypes = [C.c_uint]
    renderer.get_frame.argtypes = [C.c_void_p]
    raster_samples = []
    for frame in sorted(set(min(ch['frame'] + 75, len(scenes) + 15) for ch in chapters) | {len(scenes) + 7}):
        if not renderer.seek_frame(frame):
            raise ValueError('The C decoder could not seek to a raster sample')
        renderer.get_frame(output)
        raster_samples.append(dict(frame=frame, sha256=hashlib.sha256(output.raw).hexdigest()))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(binary)
    report = inspect(binary) | dict(rom_sha256=ROM_SHA256, core_revision=CORE_REVISION,
        controls_sha256=hashlib.sha256(controls_bytes).hexdigest(), verified_rasters=len(scenes),
        capture_patch_sha256=hashlib.sha256(patch).hexdigest(),
        capture_core_sha256=hashlib.sha256(args.core.read_bytes()).hexdigest(),
        compressed_scene_matches=len(scenes) + 16, verified_loop=True,
        decoded_scenes_sha256=decoded_hash.hexdigest(), raster_samples=raster_samples,
        raster_mismatches=0, sample_native_frames=args.sample, final_game_mode=final_mode,
        capture_seconds=round(time.monotonic() - started, 2))
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--controls', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--sample', type=int, choices=(6, 8, 10, 12), default=6)
    capture(parser.parse_args())


if __name__ == '__main__':
    main()
