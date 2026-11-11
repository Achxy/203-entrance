"""Strict ECL1 validation for local clip uploads; conversion lives in the offline UI."""
import struct
import zlib

CAPACITY = 12288


def inspect_clip(data: bytes) -> dict:
    if not 83 <= len(data) <= CAPACITY:
        raise ValueError("Clip must fit the 12 KiB storage budget")
    magic, size, crc, width, height, fps, flags, frames, reserved = struct.unpack_from("<4sIIBBBBHH", data)
    if (magic != b"ECL1" or size != len(data) or crc != zlib.crc32(data[12:]) or flags or reserved
            or not 1 <= width <= 160 or not 1 <= height <= 160 or width * height > 20480
            or not 1 <= fps <= 12 or not 1 <= frames <= min(120, fps * 60)):
        raise ValueError("Invalid clip header, bounds, or checksum")
    name = data[20:48]
    if name[-1] or any(c < 32 or c > 126 for c in name.split(b"\0", 1)[0]) or any(name[len(name.split(b"\0", 1)[0]):]):
        raise ValueError("Invalid clip label")
    pixels, at = width * height, 80
    for frame in range(frames):
        if at + 3 > len(data):
            raise ValueError("Truncated clip frame")
        length = struct.unpack_from("<H", data, at)[0]
        at += 2
        end, mode = at + length, data[at]
        at += 1
        if length < 1 or end > len(data) or (not frame and mode != 0):
            raise ValueError("Invalid clip frame")
        if mode == 0:
            if end - at != (pixels + 1) // 2 or (pixels % 2 and data[end - 1] & 15):
                raise ValueError("Invalid raw frame")
            at = end
        elif mode == 1:
            pixel = 0
            while at < end:
                token = data[at]
                at += 1
                pixel += (token & 127) + 1
                if pixel > pixels:
                    raise ValueError("Frame run overflow")
                if token & 128:
                    if at == end or data[at] > 15:
                        raise ValueError("Invalid frame color")
                    at += 1
            if pixel != pixels:
                raise ValueError("Incomplete frame")
        else:
            raise ValueError("Unsupported frame encoding")
    if at != len(data):
        raise ValueError("Unexpected trailing clip bytes")
    return {"format": "ECL1", "size": size, "crc32": f"{crc:08x}", "width": width,
            "height": height, "fps": fps, "frames": frames, "duration": frames / fps,
            "title": name.split(b"\0", 1)[0].decode("ascii")}
