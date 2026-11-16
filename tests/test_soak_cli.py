import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from tests.test_soak import lighting_status

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import entrance


class Clock:
    def __init__(self):
        self.now = 0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class SoakCLITests(unittest.TestCase):
    def run_monitor(self, renewal_seconds, *, lighting=False, lighting_change=None, minutes=60):
        clock = Clock()

        class FakeDevice:
            def __init__(self, *args, **kwargs):
                self.connection = self
                self.closed = False
                self.logins = 0
                self.operations = []
                self.lighting_reads = 0

            def login(self):
                self.logins += 1
                clock.sleep(33 if self.logins == 1 else renewal_seconds)

            def close(self):
                self.closed = True

            def request(self, method, path):
                self.operations.append((method, path))
                if (method, path) == ('GET', '/api/v1/lights'):
                    clock.sleep(1)
                    self.lighting_reads += 1
                    value = lighting_status(completed=self.lighting_reads)
                    if lighting_change:
                        value = lighting_change(self.lighting_reads, value)
                    if isinstance(value, BaseException):
                        raise value
                    return value
                if (method, path) != ('GET', '/api/v1/status'):
                    raise AssertionError('Unexpected device operation')
                clock.sleep(1)
                return dict(version='0.3.0-test', slot='ota_0',
                    uptime_ms=int(clock.now * 1000), free_heap=55000,
                    minimum_heap=32000, largest_block=32000, reset_reason=3,
                    settings_healthy=True, network=dict(settings_generation=8),
                    ota=dict(healthy=True, trial=False, busy=False),
                    display=dict(mode='replay', ready=True, rendered=int(clock.now * 10),
                        replay=dict(frame=0, loops=int(clock.now // 3)),
                        animation_slot=2, sha256='', profile='black', rotation=1,
                        inverted=False, error='', suspended=False))

        device = FakeDevice()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'soak.json'
            args = SimpleNamespace(minutes=minutes, replay=True, min_loops=2,
                output=str(output), serial=False, lighting=lighting, host='fixture', tls12=True)
            error = None
            with patch.object(entrance, 'Device', return_value=device), \
                 patch.object(entrance, 'time', clock), \
                 contextlib.redirect_stdout(io.StringIO()):
                try:
                    entrance.cmd_soak(args)
                except (ValueError, RuntimeError) as caught:
                    error = caught
            return json.loads(output.read_text()), device, error

    def test_expensive_renewal_does_not_include_the_regular_poll_sleep(self):
        report, device, error = self.run_monitor(33)
        self.assertIsNone(error)
        self.assertTrue(report['passed'])
        self.assertEqual(device.logins, 2)
        self.assertTrue(device.closed)
        gaps = [b['elapsed_s'] - a['elapsed_s']
                for a, b in zip(report['samples'], report['samples'][1:])]
        self.assertEqual(max(gaps), 34)
        self.assertGreaterEqual(report['duration_s'], 3600)
        self.assertTrue(all(path == '/api/v1/status' for _, path in device.operations))
        self.assertNotIn('lighting', report)

    def test_genuinely_late_renewal_still_fails_and_retains_evidence(self):
        report, device, error = self.run_monitor(46)
        self.assertIsNotNone(error)
        self.assertFalse(report['passed'])
        self.assertIn('Stale or missing soak telemetry', report['errors'])
        self.assertEqual(device.logins, 2)
        self.assertTrue(device.closed)
        self.assertGreater(len(report['samples']), 100)

    def test_lighting_uses_two_reads_and_renewal_preserves_the_gap_limit(self):
        report, device, error = self.run_monitor(33, lighting=True)
        self.assertIsNone(error)
        self.assertTrue(report['passed'])
        self.assertTrue(device.closed)
        self.assertEqual(device.logins, 2)
        self.assertEqual(device.operations, [('GET', '/api/v1/status'), ('GET', '/api/v1/lights')] * len(report['samples']))
        self.assertEqual(report['lighting']['observed_samples'], len(report['samples']))
        self.assertEqual(report['lighting']['minimum_worker_stack_free_bytes'], 4096)
        gaps = [b['elapsed_s'] - a['elapsed_s'] for a, b in zip(report['samples'], report['samples'][1:])]
        self.assertEqual(max(gaps), 35)
        late, device, error = self.run_monitor(44, lighting=True)
        self.assertIsNotNone(error)
        self.assertFalse(late['passed'])
        self.assertIn('Stale or missing soak telemetry', late['errors'])
        self.assertTrue(late['failed_sample']['lighting']['active'])
        self.assertTrue(device.closed)

    def test_lighting_disappearing_fails_despite_healthy_management(self):
        def disappearing(index, value):
            return dict(value, active=False, bluetooth=False, private_key='do-not-save') if index == 4 else value
        report, device, error = self.run_monitor(33, lighting=True, lighting_change=disappearing, minutes=1)
        self.assertIsNotNone(error)
        self.assertFalse(report['passed'])
        self.assertEqual(len(report['samples']), 3)
        self.assertTrue(report['failed_sample']['services'])
        self.assertFalse(report['failed_sample']['lighting']['active'])
        self.assertNotIn('do-not-save', json.dumps(report))
        self.assertEqual(device.lighting_reads, 4)
        self.assertTrue(device.closed)

    def test_missing_lighting_endpoint_never_passes_or_retries_silently(self):
        def unavailable(index, value):
            return RuntimeError('Lighting endpoint unavailable') if index == 4 else value
        report, device, error = self.run_monitor(33, lighting=True, lighting_change=unavailable, minutes=1)
        self.assertIsNotNone(error)
        self.assertFalse(report['passed'])
        self.assertIn('Lighting endpoint unavailable', report['errors'])
        self.assertEqual(device.lighting_reads, 4)
        self.assertTrue(device.closed)

    def test_serial_lighting_is_rejected_before_device_or_output_access(self):
        args = SimpleNamespace(serial=True, lighting=True)
        with patch.object(entrance, 'Device') as device, \
                patch.object(entrance, 'serial_port') as serial_port, \
                patch.object(entrance, 'private_write') as write:
            with self.assertRaisesRegex(ValueError, '--lighting requires HTTPS'):
                entrance.cmd_soak(args)
            device.assert_not_called()
            serial_port.assert_not_called()
            write.assert_not_called()


if __name__ == '__main__':
    unittest.main()
