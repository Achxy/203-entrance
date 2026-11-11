"""Plan a settings-preserving, signed USB installation for the fixed two-slot layout."""
from __future__ import annotations
import binascii
import struct
from firmware import SLOT_BYTES, validate_version

FLASH_BYTES = 0x400000
SLOT_OFFSETS = (0x20000, 0x200000)


class HealthyBoot:
    """Require one continuous healthy boot of the expected physical install."""
    def __init__(self, version: str):
        self.version = version
        self.first = None
        self.previous = None

    def observe(self, row: dict) -> bool:
        uptime = row.get('uptime_ms')
        if type(uptime) is not int or uptime < 0:
            raise ValueError('Invalid USB health uptime')
        if row.get('version') != self.version:
            raise ValueError('Unexpected firmware booted after USB installation')
        if row.get('services') is not True or row.get('trial') is not False:
            raise ValueError('USB firmware did not report healthy services outside an OTA trial')
        if self.previous is not None and uptime <= self.previous:
            raise ValueError('USB firmware restarted or repeated stale health telemetry')
        if self.first is None:
            self.first = uptime
        self.previous = uptime
        return uptime - self.first >= 30000


def signed_image_size(slot: bytes) -> int:
    if len(slot) < 24 or slot[0] != 0xE9 or not 1 <= slot[1] <= 16:
        raise ValueError('Application header is invalid')
    end = 24
    for _ in range(slot[1]):
        if end + 8 > len(slot):
            raise ValueError('Truncated application segment')
        length = struct.unpack_from('<I', slot, end + 4)[0]
        end += 8 + length
        if end >= len(slot):
            raise ValueError('Application segment exceeds its slot')
    end = (end | 15) + 1 + (32 if slot[23] else 0)
    size = (end + 4095) // 4096 * 4096 + 4096
    if size > len(slot) or slot[size-4096:size-4094] != b'\xe7\x02':
        raise ValueError('Application signature block is missing')
    return size


def newer(candidate: str, current: str) -> bool:
    def parts(value):
        validate_version(value)
        core, _, pre = value.split('+', 1)[0].partition('-')
        return tuple(map(int, core.split('.'))), pre.split('.') if pre else []
    a, ap = parts(candidate)
    b, bp = parts(current)
    if a != b:
        return a > b
    if not ap or not bp:
        return bool(bp) and not ap
    for x, y in zip(ap, bp):
        if x == y:
            continue
        if x.isdigit() and y.isdigit():
            return int(x) > int(y)
        if x.isdigit() != y.isdigit():
            return not x.isdigit()
        return x > y
    return len(ap) > len(bp)


def plan(backup: bytes, partition_table: bytes) -> dict:
    if len(backup) != FLASH_BYTES:
        raise ValueError('A complete 4 MB backup is required before USB installation')
    if not partition_table or len(partition_table) > 4096 or backup[0x8000:0x8000+len(partition_table)] != partition_table:
        raise ValueError('Partition table differs; use the documented USB reprovisioning procedure')
    valid = []
    for index in (0, 1):
        offset = 0x10000 + index * 4096
        seq, = struct.unpack_from('<I', backup, offset)
        state, crc = struct.unpack_from('<II', backup, offset + 24)
        if seq in (0, 0xFFFFFFFF) or crc != binascii.crc32(struct.pack('<I', seq), 0xFFFFFFFF):
            continue
        if state in (0, 1):
            raise ValueError('An OTA trial is pending; finish or roll it back before USB installation')
        if state in (2, 0xFFFFFFFF):
            valid.append((seq, index))
    if not valid:
        raise ValueError('No valid boot-selection record; use USB recovery instead of guessing an active slot')
    seq, index = max(valid)
    active = (seq - 1) % 2
    destination = 1 - active
    next_seq = seq + 1
    while (next_seq - 1) % 2 != destination:
        next_seq += 1
    if next_seq >= 0xFFFFFFFF:
        raise ValueError('Boot sequence exhausted; use USB reprovisioning')
    sector = bytearray(b'\xff' * 4096)
    struct.pack_into('<I', sector, 0, next_seq)
    # USB is an explicitly trusted physical installation, like the initial
    # baseline. OTA trial confirmation remains mandatory for network updates.
    struct.pack_into('<II', sector, 24, 2, binascii.crc32(struct.pack('<I', next_seq), 0xFFFFFFFF))
    return {'active_slot': active, 'destination_slot': destination,
            'application_offset': SLOT_OFFSETS[destination],
            'metadata_offset': 0x10000 + (1-index)*4096, 'metadata': bytes(sector)}
