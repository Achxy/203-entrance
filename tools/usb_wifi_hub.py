#!/usr/bin/env python3
"""Dedicated ESP32-C3 USB/Wi-Fi TCP bridge. Never configures host networking."""
from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import queue
import re
import secrets
import select
import socket
import struct
import subprocess
import sys
import threading
import time
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
MAX_DATA = 2048
HEADER = struct.Struct('<4sBBHII')
HELLO, JOIN, OPEN, DATA, ACK, CLOSE, SCAN = range(1, 8)
WIZ_QUERY, WIZ_META, WIZ_BODY, WIZ_DONE = 8, 0x89, 0x8a, 0x8b
WIZ_METHODS = ('getSystemConfig', 'getModelConfig', 'getUserConfig', 'getPilot', 'getTime')
PRIVATE_NETWORKS = tuple(ipaddress.IPv4Network(s) for s in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))


def mac_bytes(value):
    if not isinstance(value, str) or not re.fullmatch(r'(?:[0-9a-fA-F]{12}|(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2})', value):
        raise ValueError('Invalid MAC address')
    raw = bytes.fromhex(value.replace(':', ''))
    if not any(raw) or raw[0] & 1:
        raise ValueError('Invalid unicast MAC address')
    return raw


def strict_json(data, maximum=2048):
    if not isinstance(data, (bytes, str)) or len(data) > maximum:
        raise ValueError('Diagnostic JSON exceeds bound')
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('Duplicate JSON key')
            result[key] = value
        return result
    try:
        result = json.loads(data, object_pairs_hook=pairs,
                            parse_constant=lambda _value: (_ for _ in ()).throw(ValueError('Invalid JSON number')))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as exc:
        raise ValueError('Malformed diagnostic JSON') from exc
    def bounds(value, depth=0):
        if depth > (16 if maximum > 2048 else 8):
            raise ValueError('Diagnostic JSON nesting exceeds bound')
        if isinstance(value, dict):
            return 1 + sum(bounds(k, depth+1) + bounds(v, depth+1) for k, v in value.items())
        if isinstance(value, list):
            return 1 + sum(bounds(v, depth+1) for v in value)
        if isinstance(value, str) and ('\0' in value or any(0xd800 <= ord(c) <= 0xdfff for c in value)):
            raise ValueError('Invalid diagnostic JSON string')
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError('Invalid JSON number')
        return 1
    if not isinstance(result, dict) or bounds(result) > (16384 if maximum > 2048 else 512):
        raise ValueError('Invalid diagnostic JSON shape')
    return result


def home_allowlist(path):
    # The owner-provided export is private input. Only MACs escape this function;
    # no signing key, home ID, credentials or raw export are retained or emitted.
    if path.stat().st_size > 1024*1024:
        raise ValueError('Home export exceeds bound')
    export = strict_json(path.read_bytes(), 1024*1024)
    if export.get('version') != '1.0' or not isinstance(export.get('devices'), list):
        raise ValueError('Unsupported home export schema')
    devices = export['devices']
    if not 1 <= len(devices) <= 8:
        raise ValueError('Diagnostics support one to eight exported devices')
    try:
        macs = [mac_bytes(device['mac_address']) for device in devices]
    except (KeyError, TypeError) as exc:
        raise ValueError('Home export device has no valid MAC') from exc
    if len(set(macs)) != len(macs):
        raise ValueError('Home export has duplicate device MACs')
    return macs


def diagnostic_network(state):
    ip = ipaddress.IPv4Address(state['ip'])
    network = ipaddress.IPv4Network(f"{ip}/{state['netmask']}", strict=False)
    if not any(network.subnet_of(parent) for parent in PRIVATE_NETWORKS) or not 8 <= network.prefixlen <= 30 or ip in (network.network_address, network.broadcast_address):
        raise ValueError('Hub diagnostics require a private IPv4 subnet')
    return ip, network


def private_diagnostic(path, report):
    parent = path.parent
    if parent.is_symlink():
        raise ValueError('Diagnostic directory must not be a symlink')
    parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if parent.stat().st_uid != os.getuid():
        raise ValueError('Diagnostic directory must belong to this user')
    if parent.stat().st_mode & 0o077:
        raise ValueError('Diagnostic directory must be private (mode 0700)')
    fd, name = tempfile.mkstemp(prefix='.wiz-diagnostic-', dir=parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            os.fchmod(stream.fileno(), 0o600)
            json.dump(report, stream, ensure_ascii=True, indent=2, allow_nan=False)
            stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


class HubCommandError(ConnectionError):
    def __init__(self, reason):
        self.reason = reason
        super().__init__(reason)


class LinkRecovery:
    """Bound recovery to failed TCP opens; never replay application traffic."""
    def __init__(self):
        self.failures = 0
        self.retry_after = 0

    def opened(self):
        self.failures = 0

    def failed(self, reason, now):
        if reason not in ('tcp_connect_failed', 'wifi_not_ready_or_invalid_target'):
            self.failures = 0
            return False
        self.failures += 1
        if self.failures < 3 or now < self.retry_after:
            return False
        self.failures = 0
        self.retry_after = now + 60
        return True


def encode(kind, request, payload=b''):
    if len(payload) > MAX_DATA:
        raise ValueError('USB frame exceeds bound')
    prefix = struct.pack('<4sBBHI', b'203H', 1, kind, len(payload), request)
    crc = zlib.crc32(payload, zlib.crc32(prefix))
    return prefix + struct.pack('<I', crc) + payload


class Decoder:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while True:
            at = self.buffer.find(b'203H')
            if at < 0:
                self.buffer[:] = self.buffer[-3:]
                break
            if at:
                del self.buffer[:at]
            if len(self.buffer) < HEADER.size:
                break
            _, version, kind, length, request, checksum = HEADER.unpack_from(self.buffer)
            if version != 1 or length > MAX_DATA:
                raise ValueError('Invalid USB frame header')
            if len(self.buffer) < HEADER.size + length:
                break
            payload = bytes(self.buffer[HEADER.size:HEADER.size+length])
            if zlib.crc32(payload, zlib.crc32(self.buffer[:12])) != checksum:
                raise ValueError('USB frame checksum failed; connection closed')
            del self.buffer[:HEADER.size+length]
            result.append((kind, request, payload))
        return result


class Hub:
    def __init__(self, port):
        import serial
        self.serial = serial.Serial(port=None, baudrate=921600, timeout=.05,
                                    write_timeout=3, exclusive=True)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()
        self.serial.reset_input_buffer()
        self.lock = threading.Lock()
        self.write_lock = threading.Lock()
        self.pending = {}
        self.streams = {}
        self.client = None
        self.connection = 0
        self.ack = threading.Event()
        self.closed = threading.Event()
        self.failure = None
        self.reader = threading.Thread(target=self._read, name='hub-usb', daemon=True)
        self.reader.start()

    def send(self, kind, request, payload=b''):
        if self.closed.is_set():
            raise ConnectionError(self.failure or 'Hub closed')
        packet = encode(kind, request, payload)
        with self.write_lock:
            if self.serial.write(packet) != len(packet):
                raise ConnectionError('Incomplete USB write')

    def rpc(self, kind, payload=b'', *, request=None, timeout=8):
        request = request or (secrets.randbits(32) or 1)
        key = (kind | 0x80, request)
        response = queue.Queue(maxsize=1)
        with self.lock:
            if key in self.pending:
                raise RuntimeError('Duplicate USB request ID')
            self.pending[key] = response
        try:
            self.send(kind, request, payload)
            try:
                raw = response.get(timeout=timeout)
            except queue.Empty as exc:
                raise TimeoutError('Hub did not answer USB command') from exc
            if isinstance(raw, Exception):
                raise raw
            result = json.loads(raw)
            if result.get('ok') is False:
                raise HubCommandError(result['reason'])
            return result
        finally:
            with self.lock:
                self.pending.pop(key, None)

    def _read(self):
        decoder = Decoder()
        try:
            while not self.closed.is_set():
                for kind, request, payload in decoder.feed(self.serial.read(4096)):
                    with self.lock:
                        pending = self.pending.get((kind, request))
                        stream = self.streams.get(request)
                        client, connection, ack = self.client, self.connection, self.ack
                    if stream is not None and kind in (WIZ_QUERY | 128, WIZ_META, WIZ_BODY, WIZ_DONE):
                        stream.put_nowait((kind, payload))
                    elif pending is not None:
                        if pending.empty():
                            pending.put_nowait(payload)
                    elif kind == DATA and request == connection and client:
                        try:
                            client.sendall(payload)
                        except OSError:
                            if self._detach((connection, client, ack)):
                                self.send(CLOSE, connection)
                        else:
                            self.send(ACK, connection)
                    elif kind == ACK and request == connection:
                        ack.set()
                    elif kind == CLOSE and request == connection:
                        self._detach((connection, client, ack))
        except Exception as exc:
            if not self.closed.is_set():
                self.failure = f'{type(exc).__name__}: {exc}'
        finally:
            self.closed.set()
            self._detach()
            with self.lock:
                for response in self.pending.values():
                    if response.empty():
                        response.put_nowait(ConnectionError(self.failure or 'USB disconnected'))
                for response in self.streams.values():
                    try: response.put_nowait(ConnectionError('USB diagnostic stream disconnected'))
                    except queue.Full: pass

    def wiz_query(self, allowlist, state, timeout=30):
        if not 1 <= len(allowlist) <= 8 or len(set(allowlist)) != len(allowlist):
            raise ValueError('Invalid diagnostic allowlist')
        for mac in allowlist:
            if len(mac) != 6 or mac_bytes(mac.hex()) != mac:
                raise ValueError('Invalid diagnostic MAC')
        local_ip, network = diagnostic_network(state)
        request = secrets.randbits(32) or 1
        events = queue.Queue(maxsize=82)  # <=40 metadata/body pairs, start and completion.
        with self.lock:
            if self.client or self.streams:
                raise RuntimeError('Hub is busy')
            self.streams[request] = events
        records, pending, started, seen, discovered = [], None, False, set(), {}
        deadline = time.monotonic() + min(timeout, 35)
        try:
            self.send(WIZ_QUERY, request, bytes((1, len(allowlist))) + b''.join(allowlist))
            while time.monotonic() < deadline:
                try: event = events.get(timeout=max(.001, deadline-time.monotonic()))
                except queue.Empty as exc: raise TimeoutError('Hub diagnostic deadline expired') from exc
                if isinstance(event, Exception): raise event
                kind, payload = event
                if kind == WIZ_QUERY | 128:
                    if started or pending or records: raise ValueError('Duplicate diagnostic start')
                    reply = strict_json(payload)
                    if reply.get('ok') is not True: raise HubCommandError('Diagnostic request rejected')
                    started = True
                elif not started:
                    raise ValueError('Diagnostic event before start')
                elif kind == WIZ_META:
                    if pending is not None or len(payload) != 16 or payload[0] != 1 or payload[3] != 0:
                        raise ValueError('Invalid diagnostic metadata')
                    method, index = payload[1:3]
                    ip, mac, length = ipaddress.IPv4Address(payload[4:8]), payload[8:14], int.from_bytes(payload[14:16], 'little')
                    if method >= len(WIZ_METHODS) or index >= len(allowlist) or mac != allowlist[index] or not 1 <= length <= MAX_DATA:
                        raise ValueError('Diagnostic identity or length mismatch')
                    if ip not in network or ip in (local_ip, network.network_address, network.broadcast_address) or (index, method) in seen:
                        raise ValueError('Unexpected diagnostic endpoint or duplicate reply')
                    if method != 0 and discovered.get(index) != ip:
                        raise ValueError('Diagnostic endpoint differs from discovery')
                    pending = (method, index, ip, mac, length)
                elif kind == WIZ_BODY:
                    if pending is None or len(payload) != pending[4]:
                        raise ValueError('Unmatched diagnostic body')
                    method, index, ip, mac, _ = pending
                    body = strict_json(payload)
                    result, error = body.get('result'), body.get('error')
                    if (body.get('method') != WIZ_METHODS[method] or
                            (('result' in body) == ('error' in body)) or
                            not isinstance(result if 'result' in body else error, dict)):
                        raise ValueError('Diagnostic reply method or envelope mismatch')
                    returned_mac = result.get('mac') if isinstance(result, dict) else None
                    if returned_mac is not None and mac_bytes(returned_mac) != mac:
                        raise ValueError('Diagnostic reply MAC mismatch')
                    if isinstance(result, dict) and method in (0, 3) and returned_mac is None:
                        raise ValueError('Diagnostic identity absent')
                    records.append({'index': index, 'ip': str(ip), 'mac': mac.hex(), 'method': WIZ_METHODS[method], 'response': body})
                    if method == 0: discovered[index] = ip
                    seen.add((index, method)); pending = None
                elif kind == WIZ_DONE:
                    if pending is not None: raise ValueError('Incomplete diagnostic response')
                    summary = strict_json(payload)
                    if type(summary.get('replies')) is not int or summary['replies'] != len(records):
                        raise ValueError('Diagnostic reply count mismatch')
                    return {'records': records, 'summary': summary}
            raise TimeoutError('Hub diagnostic deadline expired')
        finally:
            with self.lock: self.streams.pop(request, None)

    def _detach(self, expected=None):
        with self.lock:
            if expected is not None:
                connection, client, ack = expected
                if (self.connection != connection or self.client is not client or
                        self.ack is not ack):
                    return False
            client, self.client = self.client, None
            ack = self.ack
            self.connection = 0
        if client:
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            client.close()
        ack.set()
        return True

    def close(self):
        try:
            if self.connection and not self.closed.is_set():
                self.send(CLOSE, self.connection)
        finally:
            self.closed.set()
            self._detach()
            self.reader.join(timeout=1)
            self.serial.close()

    def connect_wifi(self, profile):
        ssid = profile['ssid'].encode('utf-8')
        password = profile.get('password', '').encode('utf-8')
        if not 1 <= len(ssid) <= 32 or len(password) > 63 or b'\0' in ssid + password:
            raise ValueError('Invalid Wi-Fi credential lengths')
        self.rpc(JOIN, bytes((len(ssid), len(password))) + ssid + password)
        deadline = time.monotonic() + 45
        state = {}
        while time.monotonic() < deadline:
            state = self.rpc(HELLO)
            if state.get('connected'):
                return state
            time.sleep(1)
        raise TimeoutError(f"Hub Wi-Fi did not connect (reason {state.get('wifi_reason')})")

    def forward(self, client, target, port, on_open=None):
        request = secrets.randbits(32) or 1
        client.settimeout(5)
        # Reader callbacks and cleanup retain this generation's event even
        # after a new stream replaces the client outside the lock.
        ack = threading.Event()
        with self.lock:
            self.client, self.connection, self.ack = client, request, ack
        try:
            self.rpc(OPEN, ipaddress.IPv4Address(target).packed + struct.pack('!H', port),
                     request=request)
            if on_open:
                on_open()
            while not self.closed.is_set() and self.connection == request:
                readable, _, _ = select.select([client], [], [], .2)
                if not readable:
                    continue
                data = client.recv(MAX_DATA)
                if not data:
                    break
                ack.clear()
                self.send(DATA, request, data)
                if not ack.wait(8) or self.connection != request:
                    raise ConnectionError('Hub TCP write failed or timed out')
        except OSError:
            # The USB reader closes this descriptor when the remote TCP peer
            # closes. That normal race must not kill the radio supervisor.
            if self.connection == request:
                raise
        finally:
            try:
                if not self.closed.is_set():
                    self.send(CLOSE, request)
            finally:
                self._detach((request, client, ack))


def serve(args):
    if not 1024 <= args.listen_port <= 65535 or not 1 <= args.target_port <= 65535:
        raise ValueError('Invalid port')
    profile = json.loads(args.wifi_profile.read_text())
    hub = Hub(args.port)
    stop = threading.Event()
    active = threading.Lock()
    recovery = LinkRecovery()
    try:
        identity = hub.rpc(HELLO)
        if identity['mac'].lower() != args.expected_mac.lower():
            raise ValueError('Attached hub MAC differs from --expected-mac')
        print('Hub identity:', json.dumps(identity), flush=True)
        state = hub.connect_wifi(profile)
        print('Hub Wi-Fi connected:', json.dumps(state), flush=True)
        if args.status_file:
            save_status(args.status_file, state)

        def heartbeat():
            while not stop.wait(2):
                try:
                    state = hub.rpc(HELLO)
                    if args.status_file:
                        save_status(args.status_file, state)
                except Exception as exc:
                    print('Hub heartbeat failed:', str(exc), file=sys.stderr, flush=True)
                    stop.set()
                    break

        def connection(client):
            try:
                hub.forward(client, args.target, args.target_port, on_open=recovery.opened)
            except Exception as exc:
                print(time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                      'Tunnel connection ended:', str(exc), file=sys.stderr, flush=True)
                if isinstance(exc, HubCommandError) and recovery.failed(exc.reason, time.monotonic()):
                    # No active stream exists. Refresh only the spare's radio,
                    # with the single-client lock still held. The caller sees
                    # a closed stream and owns any safe application retry.
                    client.close()
                    print('Refreshing hub Wi-Fi after three failed TCP opens', flush=True)
                    try:
                        state = hub.connect_wifi(profile)
                        if args.status_file:
                            save_status(args.status_file, state)
                        print('Hub Wi-Fi rejoined', flush=True)
                    except Exception as reconnect_error:
                        print('Hub Wi-Fi refresh failed:', str(reconnect_error),
                              file=sys.stderr, flush=True)
            finally:
                client.close()
                active.release()

        threading.Thread(target=heartbeat, name='hub-heartbeat', daemon=True).start()
        with socket.socket() as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(('127.0.0.1', args.listen_port))
            server.listen(2)
            server.settimeout(.5)
            print(f'Ready: 127.0.0.1:{args.listen_port} -> USB -> {args.target}:{args.target_port}',
                  flush=True)
            while not stop.is_set() and not hub.closed.is_set():
                try:
                    client, _ = server.accept()
                except socket.timeout:
                    continue
                if not active.acquire(blocking=False):
                    client.close()
                    continue
                threading.Thread(target=connection, args=(client,), daemon=True).start()
        if hub.failure:
            raise ConnectionError(hub.failure)
    finally:
        stop.set()
        hub.close()


def save_status(path, state):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_suffix('.tmp')
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        json.dump(dict(state, observed_unix=time.time()), f)
    os.replace(temporary, path)


def build(_args):
    import entrance
    actual = subprocess.check_output(['git', '-C', str(entrance.SDK), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != entrance.IDF_COMMIT:
        raise ValueError('Hub must use the pinned project SDK')
    directory = ROOT / '.local/hub'
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    subprocess.run([str(entrance.runtime()), str(entrance.SDK / 'tools/idf.py'),
                    '-C', str(ROOT / 'hub/firmware'), '-B', str(directory / 'build'),
                    '-D', f'SDKCONFIG={directory / "sdkconfig"}', '-D', 'IDF_TARGET=esp32c3',
                    'build'], env=entrance.idf_env(), check=True)


def scan(args):
    hub = Hub(args.port)
    try:
        identity = hub.rpc(HELLO)
        if identity['mac'].lower() != args.expected_mac.lower():
            raise ValueError('Attached hub MAC differs from --expected-mac')
        result = hub.rpc(SCAN, args.ssid.encode(), timeout=15)
        for ap in result['aps']:
            ap['ssid'] = bytes.fromhex(ap.pop('ssid_hex')).decode('utf-8', errors='replace')
        print(json.dumps(result, indent=2))
    finally:
        hub.close()


def model_summary(model):
    """Reported bounds only; a gradient flag is not implemented gradient control."""
    summary = {}
    # Newer firmware reports its extended temperature endpoints as cctRange.
    # Preserve the original array and its source field rather than hiding the
    # difference between a legacy white range and the extended range.
    for field in ('cctRange', 'extRange', 'whiteRange'):
        if field not in model:
            continue
        white = model[field]
        if isinstance(white, list) and 2 <= len(white) <= 4 and all(type(v) is int and 1000 <= v <= 10000 for v in white):
            summary['reported_white_range'] = white
            summary['reported_white_range_field'] = field
        break
    dim = model.get('minDimLevel')
    if type(dim) is int and 1 <= dim <= 100:
        summary['reported_min_dimming'] = dim
    return summary


def wiz_query(args):
    allowlist = home_allowlist(args.home_export)
    if args.wifi_profile.stat().st_size > 4096:
        raise ValueError('Wi-Fi profile exceeds bound')
    profile = strict_json(args.wifi_profile.read_bytes(), 4096)
    if set(profile) - {'ssid', 'password'} or not isinstance(profile.get('ssid'), str) or not isinstance(profile.get('password', ''), str):
        raise ValueError('Wi-Fi profile must contain only SSID and optional password')
    expected = mac_bytes(args.expected_mac)
    hub = Hub(args.port)
    try:
        identity = hub.rpc(HELLO)
        if mac_bytes(identity.get('mac')) != expected:
            raise ValueError('Attached hub MAC differs from --expected-mac')
        if identity.get('protocol') != 1 or identity.get('firmware') != '1.1.0':
            raise ValueError('Read diagnostics require hub firmware 1.1.0')
        state = hub.connect_wifi(profile)
        del profile
        report = hub.wiz_query(allowlist, state)
        report.update({'v': 1, 'observed_unix': time.time(), 'hub_firmware': '1.1.0',
                       'trust': 'unauthenticated_local_udp', 'read_only': True})
        private_diagnostic(args.output, report)
        # Deliberately omit full MACs, IPs, home IDs, names and raw user config.
        found = {entry['index'] for entry in report['records'] if entry['method'] == 'getSystemConfig'}
        print(f"WiZ read diagnostics: {len(found)} of {len(allowlist)} exported lights replied.")
        def identifier(value):
            return value if isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,48}', value) else 'unreported'
        for index in sorted(found):
            replies = {e['method']: e['response'].get('result', {}) for e in report['records'] if e['index'] == index}
            system, model = replies.get('getSystemConfig', {}), replies.get('getModelConfig', {})
            summary = {'light': index+1, 'module': identifier(system.get('moduleName')),
                       'firmware': identifier(system.get('fwVersion'))}
            summary.update(model_summary(model))
            print(json.dumps(summary, separators=(',', ':')))
        print('Private diagnostic saved; replies are not cryptographic bulb authentication.')
        if report['summary'].get('ok') is not True:
            raise HubCommandError('Diagnostic ended early; private report saved')
    finally:
        hub.close()


def flash(args):
    import entrance
    directory = ROOT / '.local/hub/build'
    config = json.loads((directory / 'config/sdkconfig.json').read_text())
    if config.get('IDF_TARGET') != 'esp32c3' or config.get('SECURE_BOOT') or config.get('SECURE_FLASH_ENC_ENABLED'):
        raise ValueError('Unexpected hub target/security configuration')
    board = entrance.inspect_board(args.port)
    if board['mac'].lower() != args.expected_mac.lower():
        raise ValueError('Attached board differs from --expected-mac; nothing erased')
    if entrance.PROFILE.exists():
        production = json.loads(entrance.PROFILE.read_text())
        if production['mac'].lower() == board['mac'].lower():
            raise ValueError('Refusing to erase the active production device as a hub')
    backup = args.backup or entrance.cmd_backup(argparse.Namespace(port=args.port))
    metadata = json.loads((backup / 'metadata.json').read_text())
    original = (backup / 'flash.bin').read_bytes()
    if (metadata['mac'].lower() != board['mac'].lower() or len(original) != 0x400000 or
            hashlib.sha256(original).hexdigest() != metadata['sha256']):
        raise ValueError('Hub backup is incomplete or belongs to another board')
    artifacts = json.loads((directory / 'flasher_args.json').read_text())
    files = []
    for address, name in artifacts['flash_files'].items():
        file = directory / name
        if not file.is_file():
            raise ValueError(f'Missing hub image: {file}')
        files.extend([address, str(file)])
    entrance.tool('esptool', '--chip', 'esp32c3', '--port', args.port, 'erase-flash')
    entrance.tool('esptool', '--chip', 'esp32c3', '--port', args.port, '--baud', '460800',
                  'write-flash', *artifacts['write_flash_args'], *files)
    entrance.private_write(ROOT / '.local/hub/installation.json', json.dumps({
        'hub_mac': board['mac'], 'backup': str(backup.resolve()),
        'firmware_sha256': hashlib.sha256((directory / '203-usb-wifi-hub.bin').read_bytes()).hexdigest(),
        'flashed_unix': time.time(), 'efuses_changed': False,
    }, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('build').set_defaults(handler=build)
    p = sub.add_parser('scan')
    p.set_defaults(handler=scan)
    p.add_argument('--port', required=True)
    p.add_argument('--expected-mac', required=True)
    p.add_argument('--ssid', default='203 IoTitty', help='Empty string scans all nearby networks')
    p = sub.add_parser('wiz-query', help='Read-only WiZ discovery/status through the spare radio')
    p.set_defaults(handler=wiz_query)
    p.add_argument('--port', required=True)
    p.add_argument('--expected-mac', required=True)
    p.add_argument('--wifi-profile', required=True, type=Path)
    p.add_argument('--home-export', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p = sub.add_parser('flash')
    p.set_defaults(handler=flash)
    p.add_argument('--port', required=True)
    p.add_argument('--expected-mac', required=True)
    p.add_argument('--backup', type=Path, help='An existing verified full backup for this spare')
    p = sub.add_parser('serve')
    p.set_defaults(handler=serve)
    p.add_argument('--port', required=True)
    p.add_argument('--expected-mac', required=True)
    p.add_argument('--wifi-profile', required=True, type=Path)
    p.add_argument('--listen-port', default=18443, type=int)
    p.add_argument('--target', default='192.168.4.1')
    p.add_argument('--target-port', default=443, type=int)
    p.add_argument('--status-file', type=Path)
    args = parser.parse_args()
    try:
        args.handler(args)
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        parser.exit(1, f'Hub error: {exc}\n')


if __name__ == '__main__':
    main()
