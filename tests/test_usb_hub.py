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
import json
import os
import io
import contextlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import usb_wifi_hub as hub


class HubConnectionLifetimeTests(unittest.TestCase):
    class ClientDouble:
        def __init__(self):
            self.closed = False
            self.received = []
            self.before_shutdown = None

        def sendall(self, payload):
            if self.closed:
                raise OSError('Old local stream is closed')
            self.received.append(payload)

        def settimeout(self, _timeout):
            pass

        def shutdown(self, _how):
            if self.before_shutdown:
                self.before_shutdown()

        def close(self):
            self.closed = True

    def make_hub(self):
        # Do not open serial ports or sockets: exercise the actual reader and
        # detach implementation using an in-memory frame source.
        device = hub.Hub.__new__(hub.Hub)
        device.lock = threading.Lock()
        device.pending, device.streams = {}, {}
        device.client = self.ClientDouble()
        device.connection = 101
        device.ack, device.closed = threading.Event(), threading.Event()
        device.failure = None
        device.sent = []
        device.send = lambda kind, request, payload=b'': device.sent.append((kind, request, payload))
        return device

    def assert_stale_reader_preserves_replacement(self, kind):
        device = self.make_hub()
        old_client, old_ack = device.client, device.ack
        snapshot_ready, resume, processed = (threading.Event() for _ in range(3))
        reader = None

        class SnapshotLock:
            def __init__(self):
                self.lock = threading.Lock()
                self.pause_once = True

            def __enter__(self):
                self.lock.acquire()

            def __exit__(self, *_exc):
                self.lock.release()
                if threading.current_thread() is reader and self.pause_once:
                    self.pause_once = False
                    snapshot_ready.set()
                    if not resume.wait(2):
                        raise TimeoutError('Test did not release reader snapshot')

        class SerialDouble:
            def __init__(self):
                self.first = True

            def read(self, _length):
                if self.first:
                    self.first = False
                    return hub.encode(kind, 101, b'old response' if kind == hub.DATA else b'')
                processed.set()
                device.closed.wait(.02)
                return b''

        device.lock, device.serial = SnapshotLock(), SerialDouble()
        reader = threading.Thread(target=device._read)
        reader.start()
        try:
            self.assertTrue(snapshot_ready.wait(2), 'Reader must capture the old stream')
            device._detach()
            old_ack.clear()
            replacement, replacement_ack = self.ClientDouble(), threading.Event()
            with device.lock:
                device.client, device.connection, device.ack = replacement, 202, replacement_ack
            resume.set()
            self.assertTrue(processed.wait(2), 'Reader must finish the stale frame')
            self.assertIsNone(device.failure)
            self.assertTrue(old_client.closed)
            self.assertIs(device.client, replacement)
            self.assertEqual(device.connection, 202)
            self.assertFalse(replacement.closed)
            self.assertFalse(replacement_ack.is_set(), 'Old work must not acknowledge the new stream')
            self.assertEqual(replacement.received, [])
            self.assertEqual(device.sent, [])
            if kind == hub.ACK:
                self.assertTrue(old_ack.is_set(), 'ACK belongs to its captured stream')
        finally:
            device.closed.set()
            resume.set()
            reader.join(2)
            self.assertFalse(reader.is_alive())

    def test_stale_close_does_not_detach_replacement(self):
        self.assert_stale_reader_preserves_replacement(hub.CLOSE)

    def test_stale_data_send_failure_does_not_detach_replacement(self):
        self.assert_stale_reader_preserves_replacement(hub.DATA)

    def test_stale_ack_does_not_release_replacement_backpressure(self):
        self.assert_stale_reader_preserves_replacement(hub.ACK)

    def test_detach_requires_object_ownership_even_if_request_id_repeats(self):
        device = self.make_hub()
        old_owner = (device.connection, device.client, device.ack)
        replacement, replacement_ack = self.ClientDouble(), threading.Event()
        device.client, device.ack = replacement, replacement_ack
        self.assertFalse(device._detach(old_owner))
        self.assertIs(device.client, replacement)
        self.assertEqual(device.connection, old_owner[0])
        self.assertFalse(replacement.closed)
        self.assertFalse(replacement_ack.is_set())
        device._detach()

    def test_forward_finally_preserves_replacement_and_uses_fresh_ack(self):
        device = self.make_hub()
        client, previous_ack = device.client, device.ack
        previous_ack.set()
        replacement, replacement_ack = self.ClientDouble(), threading.Event()

        def opened(_kind, _payload, *, request):
            self.assertEqual(request, 101)
            self.assertIsNot(device.ack, previous_ack)
            self.assertFalse(device.ack.is_set())
            with device.lock:
                device.client, device.connection, device.ack = replacement, 202, replacement_ack
            return {'ok': True}

        device.rpc = opened
        with patch.object(hub.secrets, 'randbits', return_value=101):
            device.forward(client, '192.168.4.1', 443)
        self.assertIs(device.client, replacement)
        self.assertFalse(replacement.closed)
        self.assertFalse(replacement_ack.is_set())
        self.assertEqual(device.sent, [(hub.CLOSE, 101, b'')])
        device._detach()

    def test_slow_detach_does_not_acknowledge_replacement(self):
        device = self.make_hub()
        old_client, old_ack = device.client, device.ack
        shutdown_started, finish_shutdown = threading.Event(), threading.Event()
        errors = []

        def pause_shutdown():
            shutdown_started.set()
            if not finish_shutdown.wait(2):
                raise TimeoutError('Test did not release old shutdown')

        def detach():
            try:
                device._detach()
            except Exception as exc:
                errors.append(exc)

        old_client.before_shutdown = pause_shutdown
        worker = threading.Thread(target=detach)
        worker.start()
        try:
            self.assertTrue(shutdown_started.wait(2))
            replacement, replacement_ack = self.ClientDouble(), threading.Event()
            with device.lock:
                device.client, device.connection, device.ack = replacement, 202, replacement_ack
            finish_shutdown.set()
            worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertEqual(errors, [])
            self.assertTrue(old_client.closed)
            self.assertTrue(old_ack.is_set())
            self.assertIs(device.client, replacement)
            self.assertFalse(replacement.closed)
            self.assertFalse(replacement_ack.is_set())
        finally:
            finish_shutdown.set()
            worker.join(2)
            device._detach()


class HubFrameTests(unittest.TestCase):
    def test_actual_model_temperature_summary(self):
        fixture = ROOT/'lights/firmware/wiz/tests/fixtures/esp25_shrgb_01_1.38.0_model.json'
        reply = hub.strict_json(fixture.read_bytes())
        self.assertEqual(hub.model_summary(reply['result']), {
            'reported_white_range': [2200, 2700, 6500, 6500],
            'reported_white_range_field': 'cctRange', 'reported_min_dimming': 10})
        self.assertEqual(hub.model_summary({'whiteRange':[2700,6500]})['reported_white_range_field'], 'whiteRange')
        self.assertEqual(hub.model_summary({'extRange':[2200,2700,6500,6500]})['reported_white_range_field'], 'extRange')
        for value in ([True,6500], [500,6500], [2200,12000], [2200], '2200,6500'):
            self.assertNotIn('reported_white_range', hub.model_summary({'cctRange':value}))
        self.assertEqual(hub.model_summary({'hasGradient':1, 'nowc':1}), {})

    def test_wiz_core_bounds_and_read_only_state_machine(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / 'hub-wiz'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'hub/firmware/main'),
                            '-I', str(ROOT / 'managed_components/espressif__cjson/cJSON'),
                            str(ROOT / 'tests/native/hub_wiz_test.c'),
                            str(ROOT / 'hub/firmware/main/wiz_diag.c'),
                            str(ROOT / 'managed_components/espressif__cjson/cJSON/cJSON.c'),
                            '-o', str(binary)], check=True)
            result = subprocess.check_output([str(binary)], timeout=30)
            self.assertIn(b'PASS bounded read-only', result)

    def test_private_export_extracts_only_allowed_macs(self):
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / 'export.json'
            value = {'version': '1.0', 'udp_signing_key': 'TEST-KEY-NEVER-USED',
                     'home_id': 'TEST-HOME', 'devices': [{'mac_address': '44:00:00:00:00:01', 'traits': ['color']}]}
            path.write_text(json.dumps(value))
            self.assertEqual(hub.home_allowlist(path), [bytes.fromhex('440000000001')])
            value['devices'] *= 2
            path.write_text(json.dumps(value))
            with self.assertRaises(ValueError): hub.home_allowlist(path)
            path.write_text('{"version":"1.0","version":"1.0","devices":[]}')
            with self.assertRaises(ValueError): hub.home_allowlist(path)
            value['devices'] = [{'mac_address': 'ffffffffffff'}]
            path.write_text(json.dumps(value))
            with self.assertRaises(ValueError): hub.home_allowlist(path)

    def test_wiz_stream_identity_bounds_and_pairing(self):
        class SerialDouble:
            bad_endpoint = False
            def __init__(self, **_kwargs):
                self.input = queue.Queue(); self.decoder = hub.Decoder()
            def open(self): pass
            def close(self): pass
            def reset_input_buffer(self): pass
            def read(self, _n):
                try: return self.input.get(timeout=.02)
                except queue.Empty: return b''
            def write(self, data):
                for kind, request, payload in self.decoder.feed(data):
                    if kind != hub.WIZ_QUERY: raise AssertionError('Unexpected command')
                    if payload != b'\1\1' + bytes.fromhex('440000000001'): raise AssertionError('Unexpected allowlist')
                    body = b'{"method":"getSystemConfig","result":{"mac":"440000000001","moduleName":"ESP24_SHRGB_01"}}'.ljust(2048)
                    ip = b'\x08\x08\x08\x08' if self.bad_endpoint else b'\xc0\xa8\x2b\x03'
                    meta = b'\1\0\0\0' + ip + bytes.fromhex('440000000001') + len(body).to_bytes(2, 'little')
                    events = [(hub.WIZ_QUERY | 128, b'{"ok":true}'),
                              (hub.WIZ_META, meta), (hub.WIZ_BODY, body),
                              (hub.WIZ_DONE, b'{"ok":true,"replies":1}')]
                    self.input.put(b''.join(hub.encode(k, request, p) for k, p in events))
                return len(data)
        state = {'ip': '192.168.43.2', 'netmask': '255.255.255.0'}
        for bad in (False, True):
            SerialDouble.bad_endpoint = bad
            with patch.dict(sys.modules, serial=types.SimpleNamespace(Serial=SerialDouble)):
                device = hub.Hub('fake-usb')
            try:
                if bad:
                    with self.assertRaises(ValueError): device.wiz_query([bytes.fromhex('440000000001')], state)
                else:
                    report = device.wiz_query([bytes.fromhex('440000000001')], state)
                    self.assertEqual(report['records'][0]['method'], 'getSystemConfig')
                    self.assertEqual(report['summary']['replies'], 1)
                self.assertEqual(device.streams, {})
            finally: device.close()

    def test_private_diagnostic_and_console_redaction(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = pathlib.Path(temp)
            export, profile = directory/'home.json', directory/'wifi.json'
            export.write_text(json.dumps({'version':'1.0', 'udp_signing_key':'PRIVATE-EXPORT-KEY',
                                         'devices':[{'mac_address':'440000000001'}]}))
            profile.write_text(json.dumps({'ssid':'PRIVATE-NETWORK','password':'PRIVATE-PASSWORD'}))
            output = directory/'private'/'report.json'
            class FakeHub:
                def __init__(self, _port): pass
                def rpc(self, kind): return {'protocol':1,'firmware':'1.1.0','mac':'440000000099'}
                def connect_wifi(self, received):
                    self.received = received
                    return {'ip':'192.168.43.2','netmask':'255.255.255.0'}
                def wiz_query(self, allowed, state):
                    return {'summary':{'ok':True,'replies':1},'records':[{
                        'index':0,'mac':allowed[0].hex(),'ip':'192.168.43.3','method':'getSystemConfig',
                        'response':{'method':'getSystemConfig','result':{'mac':allowed[0].hex(),
                        'homeId':'PRIVATE-HOME','moduleName':'ESP24_SHRGB_01','fwVersion':'1.36.1'}}}]}
                def close(self): pass
            args=types.SimpleNamespace(port='fake-usb',expected_mac='440000000099',
                                       wifi_profile=profile,home_export=export,output=output)
            printed=io.StringIO()
            with patch.object(hub,'Hub',FakeHub),contextlib.redirect_stdout(printed): hub.wiz_query(args)
            for secret in ('PRIVATE-EXPORT-KEY','PRIVATE-NETWORK','PRIVATE-PASSWORD','PRIVATE-HOME','440000000001','192.168.43.3'):
                self.assertNotIn(secret,printed.getvalue())
            self.assertIn('ESP24_SHRGB_01',printed.getvalue())
            self.assertNotIn('PRIVATE-EXPORT-KEY',output.read_text())
            self.assertEqual(output.stat().st_mode & 0o777,0o600)
            self.assertEqual(output.parent.stat().st_mode & 0o777,0o700)
            os.chmod(output.parent,0o755)
            with self.assertRaises(ValueError): hub.private_diagnostic(output,{'safe':True})
            self.assertEqual(output.parent.stat().st_mode & 0o777,0o755)
            args.expected_mac='440000000098'
            with patch.object(hub,'Hub',FakeHub):
                with self.assertRaises(ValueError): hub.wiz_query(args)

    def test_diagnostic_rejects_public_networks_and_bad_json(self):
        for ip, mask in [('8.8.8.8','255.255.255.0'),('127.0.0.1','255.255.255.0'),
                         ('192.168.4.0','255.255.255.0'),('192.168.4.255','255.255.255.0')]:
            with self.assertRaises(ValueError): hub.diagnostic_network({'ip':ip,'netmask':mask})
        for data in (b'{"a":1,"\\u0061":2}',b'{"a":NaN}',b'{"a":1e999}',b'\xff',b'{"a":"\\u0000"}',b'{}{}'):
            with self.assertRaises(ValueError): hub.strict_json(data)

    def test_link_recovery_is_bounded_and_only_for_connect_failures(self):
        recovery = hub.LinkRecovery()
        self.assertFalse(recovery.failed('tcp_connect_failed', 0))
        self.assertFalse(recovery.failed('tcp_connect_failed', 1))
        self.assertTrue(recovery.failed('tcp_connect_failed', 2))
        for at in range(3, 62):
            self.assertFalse(recovery.failed('wifi_not_ready_or_invalid_target', at))
        self.assertTrue(recovery.failed('tcp_connect_failed', 62))
        recovery.opened()
        self.assertFalse(recovery.failed('tcp_connect_failed', 125))
        self.assertFalse(recovery.failed('tcp_connect_failed', 126))
        self.assertFalse(recovery.failed('socket_allocation_failed', 127))
        self.assertFalse(recovery.failed('tcp_connect_failed', 128))
        recovery.opened()
        self.assertFalse(recovery.failed('tcp_connect_failed', 129))
        self.assertFalse(recovery.failed('tcp_connect_failed', 130))
        self.assertTrue(recovery.failed('tcp_connect_failed', 131))

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
