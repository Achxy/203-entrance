import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import queue
import socket
import threading
import types

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import usb_wifi_hub as hub


class HubFrameTests(unittest.TestCase):
    def test_proxy_bytes_reconnect_and_session_isolation(self):
        class SerialDouble:
            def __init__(self, **_kwargs):
                self.input = queue.Queue()
                self.decoder = hub.Decoder()
                self.connection = 0
            def open(self): pass
            def close(self): pass
            def reset_input_buffer(self): pass
            def read(self, _n):
                try: return self.input.get(timeout=.02)
                except queue.Empty: return b''
            def write(self, data):
                for kind, request, payload in self.decoder.feed(data):
                    if kind in (hub.HELLO, hub.OPEN):
                        if kind == hub.OPEN: self.connection = request
                        self.input.put(hub.encode(kind | 128, request, b'{"ok":true}'))
                    elif kind == hub.DATA and request == self.connection:
                        self.input.put(hub.encode(hub.ACK, request))
                        # An old TCP session must not leak bytes to a new one.
                        self.input.put(hub.encode(hub.DATA, request ^ 1, b'stale'))
                        self.input.put(hub.encode(hub.DATA, request, payload))
                    elif kind == hub.CLOSE:
                        self.input.put(hub.encode(hub.CLOSE, request))
                return len(data)
        with patch.dict(sys.modules, serial=types.SimpleNamespace(Serial=SerialDouble)):
            device = hub.Hub('fake-usb')
        try:
            for cycle in range(3):
                local, remote = socket.socketpair()
                local.settimeout(3)
                errors = []
                def forward():
                    try: device.forward(remote, '192.168.4.1', 443)
                    except Exception as exc: errors.append(exc)
                worker = threading.Thread(target=forward)
                worker.start()
                payload = bytes(range(256)) * (17 + cycle)
                local.sendall(payload)
                received = bytearray()
                while len(received) < len(payload):
                    received.extend(local.recv(len(payload)-len(received)))
                self.assertEqual(bytes(received), payload)
                local.close()
                worker.join(timeout=3)
                self.assertFalse(worker.is_alive())
                self.assertEqual(errors, [])
                self.assertFalse(device.closed.is_set())
                self.assertEqual(device.rpc(hub.HELLO), {'ok': True})
        finally:
            device.close()

    def test_c_python_interoperability_every_frame_size(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / 'hub-frame'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'hub/firmware/main'),
                            str(ROOT / 'tests/native/hub_frame_test.c'), '-o', str(binary)], check=True)
            data = subprocess.check_output([str(binary)], timeout=30)
        decoder = hub.Decoder()
        frames = []
        for offset in range(0, len(data), 257):
            frames.extend(decoder.feed(data[offset:offset+257]))
        self.assertEqual(len(frames), 2049)
        for n, (kind, request, payload) in enumerate(frames):
            expected = bytes((i * 37 + n) & 255 for i in range(n))
            self.assertEqual((kind, request, payload), (hub.DATA, 0x12345678, expected))
        self.assertEqual(len(decoder.buffer), 0)

    def test_boot_noise_chunking_and_multiple_packets(self):
        packets = [hub.encode(hub.HELLO, 17), hub.encode(hub.DATA, 9, b'203H'*512),
                   hub.encode(hub.CLOSE, 9)]
        for size in (1, 2, 3, 4, 15, 16, 17, 64, 4096):
            decoder = hub.Decoder(); output = []
            data = b'ESP-ROM boot\r\n203partial\n' + b''.join(packets)
            for offset in range(0, len(data), size):
                output.extend(decoder.feed(data[offset:offset+size]))
            self.assertEqual(output, [(hub.HELLO, 17, b''), (hub.DATA, 9, b'203H'*512),
                                      (hub.CLOSE, 9, b'')])

    def test_rejects_corruption_and_unbounded_frames(self):
        packet = hub.encode(hub.DATA, 20, bytes(range(255)))
        for offset in range(4, len(packet)):
            corrupt = bytearray(packet); corrupt[offset] ^= 128
            decoder = hub.Decoder()
            try:
                result = decoder.feed(corrupt)
            except ValueError:
                continue
            self.assertEqual(result, []) # corrupted length can require more bytes
        with self.assertRaises(ValueError):
            hub.encode(hub.DATA, 20, bytes(2049))
        oversized = bytearray(packet); oversized[6:8] = b'\xff\xff'
        with self.assertRaises(ValueError):
            hub.Decoder().feed(oversized)
        decoder = hub.Decoder()
        self.assertEqual(decoder.feed(b'x'*100000), [])
        self.assertLessEqual(len(decoder.buffer), 3)
