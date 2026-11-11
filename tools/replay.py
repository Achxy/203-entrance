"""RPL1: bounded scene replay packing. ROM CPU/program bytes are never included."""
from __future__ import annotations
import argparse
import binascii
import hashlib
import json
import struct
import zlib
from pathlib import Path

SCENE_BYTES = 2432
BLOCK_BYTES = 16384
MAX_BYTES = 737280


def varint(n):
    if not 0 <= n <= SCENE_BYTES:
        raise ValueError('Scene offset is out of range')
    return bytes((n,)) if n < 128 else bytes(((n & 127) | 128, n >> 7))


def delta(previous, current):
    at = last = 0
    out = bytearray()
    while at < SCENE_BYTES:
        if previous[at] == current[at]:
            at += 1
            continue
        start, end = at, at + 1
        while end < SCENE_BYTES:
            if previous[end] != current[end] or any(previous[k] != current[k] for k in range(end, min(end+4, SCENE_BYTES))):
                end += 1
            else:
                break
        # Modular differences compress constant camera/sprite velocities well.
        out += varint(start-last) + varint(end-start)
        out += bytes((current[k]-previous[k]) & 255 for k in range(start, end))
        last = at = end
    return bytes(out)


def pack(scenes, tiles, palette, chapters, period_us):
    if not scenes or len(scenes) > 100000 or any(len(s) != SCENE_BYTES for s in scenes):
        raise ValueError('Invalid scene sequence')
    if len(tiles) != 8192 or len(palette) != 128 or not 50000 <= period_us <= 1000000:
        raise ValueError('Invalid tile/palette/timing data')
    if not 1 <= len(chapters) <= 32 or chapters[0]['frame'] != 0:
        raise ValueError('Expected 1–32 ordered chapters, starting at frame zero')
    blocks = []
    raw = bytearray(scenes[0]); first = 0; count = 1
    for i in range(1, len(scenes)):
        difference = delta(scenes[i-1], scenes[i])
        record = struct.pack('<H', len(difference)) + difference
        if len(raw) + len(record) > BLOCK_BYTES or count == 128:
            blocks.append((first, count, bytes(raw)))
            first, count, raw = i, 1, bytearray(scenes[i])
        else:
            raw += record; count += 1
    blocks.append((first, count, bytes(raw)))
    index_offset = 64 + 8192 + 128
    chapter_offset = index_offset + len(blocks)*16
    data_offset = chapter_offset + len(chapters)*8
    payload = bytearray(); index = bytearray()
    for first, count, raw in blocks:
        compressed = zlib.compress(raw, 9)
        index += struct.pack('<IIHHHH', first, data_offset+len(payload), len(compressed), len(raw), count, 0)
        payload += compressed
    table = bytearray(); previous = -1
    for chapter in chapters:
        frame, world, level = (chapter[k] for k in ('frame', 'world', 'level'))
        if not previous < frame < len(scenes) or not 1 <= world <= 8 or not 1 <= level <= 4:
            raise ValueError('Invalid chapter')
        table += struct.pack('<IBBBB', frame, world-1, level-1, 0, 0); previous = frame
    header = bytearray(64)
    struct.pack_into('<4sIIHHIIHHHHIIIII', header, 0, b'RPL1', data_offset+len(payload), 0,
                     1, SCENE_BYTES, len(scenes), period_us, BLOCK_BYTES, len(blocks), len(chapters), 0,
                     64, 64+8192, index_offset, chapter_offset, data_offset)
    result = header + tiles + palette + index + table + payload
    if len(result) > MAX_BYTES:
        raise ValueError(f'Replay needs {len(result)} bytes; limit is {MAX_BYTES}. Reduce sampling rate or recorded scope.')
    struct.pack_into('<I', result, 8, binascii.crc32(result[12:]))
    return bytes(result)


def inspect(data):
    if len(data) < 64 or len(data) > MAX_BYTES or data[:4] != b'RPL1' or struct.unpack_from('<I', data, 4)[0] != len(data):
        raise ValueError('Invalid RPL1 size/header')
    if struct.unpack_from('<I', data, 8)[0] != binascii.crc32(data[12:]):
        raise ValueError('Replay checksum mismatch')
    frames, period = struct.unpack_from('<II', data, 16)
    return {'bytes': len(data), 'frames': frames, 'fps': 1000000/period,
            'duration_seconds': frames*period/1000000,
            'chapters': struct.unpack_from('<H', data, 28)[0],
            'sha256': hashlib.sha256(data).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect(args.file.read_bytes()), indent=2))

if __name__ == '__main__':
    main()
