import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from soak import SoakWindow, lighting_sample


def sample(uptime=100000, heap=135000, rendered=1, loops=0):
    return dict(version='0.2.0', slot='ota_0', uptime_ms=uptime, services=True, trial=False,
                free_heap=heap, largest_block=100000, settings_generation=1,
                display=dict(mode='replay', ready=True, rendered=rendered, loops=loops))


def lighting_status(**patch):
    result = dict(compiled=True, configured=True, active=True, bluetooth=True,
        storage_healthy=True, worker_running=True, worker_stack_sample_valid=True,
        provisioning=False, paused=False, quarantined=False, resource_blocked=False,
        provision_result=0, worker_stack_min_free_bytes=4096, free_heap=54000,
        largest_block=32000, sessions=0, completed=0,
        local_time=dict(state='ready', dns_replies=0, ntp_replies=0, malformed=0,
            foreign_peer=0, rate_limited=0, clock_refused=0, io_errors=0))
    result.update(patch)
    return result


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

    def test_lighting_must_be_running_even_when_management_is_healthy(self):
        cases = [dict(compiled=False), dict(configured=False), dict(active=False),
                 dict(bluetooth=False), dict(storage_healthy=False), dict(worker_running=False),
                 dict(worker_stack_sample_valid=False), dict(provisioning=True), dict(paused=True),
                 dict(quarantined=True), dict(resource_blocked=True), dict(provision_result=-1),
                 dict(provision_result=None), dict(provision_result=False), dict(active='true'),
                 dict(free_heap=-1), dict(largest_block=1.0), dict(worker_stack_min_free_bytes=True)]
        for patch in cases:
            with self.subTest(patch=patch):
                check = SoakWindow(60, lighting=True)
                check.observe(dict(sample(), lighting=lighting_status()), 0)
                with self.assertRaises(ValueError):
                    check.observe(dict(sample(115000), lighting=lighting_status(**patch)), 15)
                report = check.report(complete=True)
                self.assertFalse(report['passed'])
                self.assertEqual(len(report['samples']), 1)
                self.assertTrue(report['failed_sample']['services'])

    def test_lighting_sanitizes_failed_evidence_and_does_not_invent_stack_threshold(self):
        check = SoakWindow(15, lighting=True)
        first = lighting_status(worker_stack_min_free_bytes=1, admin_password='do-not-save')
        first['local_time']['private_key'] = 'do-not-save'
        check.observe(dict(sample(), lighting=first), 0)
        second = lighting_status(worker_stack_min_free_bytes=0, free_heap=52000, largest_block=30000)
        second['local_time']['state'] = 'waiting_trusted_clock'
        second['local_time']['clock_refused'] = 7
        self.assertTrue(check.observe(dict(sample(115000), lighting=second), 15))
        report = check.report(complete=True)
        self.assertTrue(report['passed'])
        self.assertEqual(report['lighting']['minimum_worker_stack_free_bytes'], 0)
        self.assertEqual(report['lighting']['minimum_free_heap'], 52000)
        self.assertEqual(report['lighting']['minimum_largest_block'], 30000)
        self.assertNotIn('do-not-save', str(report))
        bad = lighting_status(active=False, home_key='do-not-save')
        bad['local_time']['state'] = 'do-not-save'
        with self.assertRaises(ValueError):
            check.observe(dict(sample(130000), lighting=bad), 30)
        report = check.report(complete=True)
        self.assertFalse(report['passed'])
        self.assertIsNone(report['failed_sample']['lighting']['local_time']['state'])
        self.assertNotIn('do-not-save', str(report))

    def test_invalid_or_missing_lighting_metrics_cannot_qualify(self):
        for value in (None, {}, {'compiled': False}, lighting_status(local_time={}),
                      lighting_status(completed='untrusted text')):
            with self.subTest(value=value):
                check = SoakWindow(15, lighting=True)
                with self.assertRaises(ValueError):
                    check.observe(dict(sample(), lighting=value), 0)
                self.assertFalse(check.report(complete=True)['passed'])
        check = SoakWindow(15, lighting=True)
        with self.assertRaises(ValueError):
            check.observe(dict(sample(), lighting=lighting_status(
                worker_stack_sample_valid=False, worker_stack_min_free_bytes=0)), 0)
        self.assertIsNone(check.report()['lighting']['minimum_worker_stack_free_bytes'])
        optional = lighting_status()
        del optional['sessions']; del optional['completed']
        self.assertNotIn('sessions', lighting_sample(optional))
        check = SoakWindow(15, lighting=True)
        check.observe(dict(sample(), lighting=optional), 0)
        check.observe(dict(sample(115000), lighting=optional), 15)
        self.assertTrue(check.report(complete=True)['passed'])
