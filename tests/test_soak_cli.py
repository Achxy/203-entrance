import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

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
    def run_monitor(self, renewal_seconds):
        clock = Clock()

        class FakeDevice:
            def __init__(self, *args, **kwargs):
                self.connection = self
                self.closed = False
                self.logins = 0

            def login(self):
                self.logins += 1
                clock.sleep(33 if self.logins == 1 else renewal_seconds)

            def close(self):
                self.closed = True

            def request(self, method, path):
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
            args = SimpleNamespace(minutes=60, replay=True, min_loops=2,
                output=str(output), serial=False, host='fixture', tls12=True)
            error = None
            with patch.object(entrance, 'Device', return_value=device), \
                 patch.object(entrance, 'time', clock), \
                 contextlib.redirect_stdout(io.StringIO()):
                try:
                    entrance.cmd_soak(args)
                except ValueError as caught:
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

    def test_genuinely_late_renewal_still_fails_and_retains_evidence(self):
        report, device, error = self.run_monitor(46)
        self.assertIsNotNone(error)
        self.assertFalse(report['passed'])
        self.assertIn('Stale or missing soak telemetry', report['errors'])
        self.assertEqual(device.logins, 2)
        self.assertTrue(device.closed)
        self.assertGreater(len(report['samples']), 100)


if __name__ == '__main__':
    unittest.main()
