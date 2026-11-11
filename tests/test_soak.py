import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from soak import SoakWindow


def sample(uptime=100000, heap=135000, rendered=1, loops=0):
    return dict(version='0.2.0', slot='ota_0', uptime_ms=uptime, services=True, trial=False,
                free_heap=heap, largest_block=100000, settings_generation=1,
                display=dict(mode='replay', ready=True, rendered=rendered, loops=loops))


class SoakTests(unittest.TestCase):
    def test_startup_and_last_partial_interval_do_not_count(self):
        check = SoakWindow(60)
        self.assertFalse(check.observe(dict(sample(100), services=False), 0))
        for index in range(5):
            self.assertEqual(check.observe(sample(15000+index*15000), 15+index*15), index == 4)
            self.assertEqual(check.report(complete=True)['passed'], index == 4)

    def test_resets_changes_unhealthy_and_stale_samples_fail(self):
        for patch in ({'uptime_ms': 100000}, {'version': '0.2.1'}, {'slot': 'ota_1'},
                      {'settings_generation': 2}, {'services': False}, {'trial': True}):
            check = SoakWindow(60)
            check.observe(sample(), 0)
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                check.observe(dict(sample(115000), **patch), 15)
        check = SoakWindow(60)
        check.observe(sample(), 0)
        with self.assertRaises(ValueError): check.observe(sample(160000), 60)

    def test_replay_must_advance_and_cross_required_boundaries(self):
        check = SoakWindow(60, replay=True, min_loops=2)
        for index in range(5):
            check.observe(sample(100000+index*15000, rendered=index+1, loops=index//2), index*15)
        self.assertTrue(check.report(complete=True)['passed'])
        check = SoakWindow(15, replay=True, min_loops=2)
        check.observe(sample(), 0)
        with self.assertRaises(ValueError): check.observe(sample(115000), 15)
        check.observe(sample(115000, rendered=2), 15)
        self.assertFalse(check.report(complete=True)['passed'])

    def test_progressive_heap_loss_fails_even_with_healthy_telemetry(self):
        check = SoakWindow(300)
        for index in range(21):
            check.observe(sample(100000+index*15000, heap=135000-index*500), index*15)
        self.assertFalse(check.report(complete=True)['passed'])

    def test_display_pause_is_not_a_replay_soak(self):
        check = SoakWindow(60, replay=True)
        row = copy.deepcopy(sample())
        row['display']['mode'] = 'replay_pause'
        with self.assertRaises(ValueError): check.observe(row, 0)
