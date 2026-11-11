import binascii
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from usb_install import HealthyBoot, initial_boot_record, plan, newer, signed_image_size

class USBInstallTests(unittest.TestCase):
    def test_initial_baseline_supports_next_update_and_failed_trial_fallback(self):
        flash = bytearray(b'\xff' * 0x400000)
        flash[0x8000:0x8004] = b'test'
        flash[0x10000:0x11000] = initial_boot_record()
        operation = plan(flash, b'test')
        self.assertEqual(operation['active_slot'], 0)
        self.assertEqual(operation['destination_slot'], 1)
        self.assertEqual(operation['metadata_offset'], 0x11000)
        flash[0x11000:0x12000] = operation['metadata']
        struct.pack_into('<I', flash, 0x11018, 1)  # Pending OTA trial.
        with self.assertRaises(ValueError): plan(flash, b'test')
        struct.pack_into('<I', flash, 0x11018, 3)  # Invalid candidate.
        self.assertEqual(plan(flash, b'test')['active_slot'], 0)

    def test_continuous_expected_healthy_boot(self):
        check=HealthyBoot('0.2.0')
        for uptime, expected in ((15000,False),(30000,False),(45000,True)):
            self.assertEqual(check.observe(dict(version='0.2.0',uptime_ms=uptime,services=True,trial=False)),expected)

    def test_startup_initialization_precedes_continuous_health(self):
        check = HealthyBoot('0.2.0')
        self.assertFalse(check.observe(dict(version='0.2.0',uptime_ms=200,services=False,trial=False)))
        for uptime, expected in ((15000,False),(30000,False),(45000,True)):
            self.assertEqual(check.observe(dict(version='0.2.0',uptime_ms=uptime,services=True,trial=False)),expected)
        overdue = HealthyBoot('0.2.0')
        with self.assertRaises(ValueError):
            overdue.observe(dict(version='0.2.0',uptime_ms=30000,services=False,trial=False))

    def test_restart_unhealthy_wrong_version_and_stale_boot_fail(self):
        good=dict(version='0.2.0',uptime_ms=15000,services=True,trial=False)
        for patch in ({'uptime_ms':1},{'uptime_ms':15000},{'uptime_ms':'45000'},
                      {'version':'0.1.0'},{'services':False},{'trial':True}):
            check=HealthyBoot('0.2.0');check.observe(good)
            with self.assertRaises(ValueError):check.observe(good|{'uptime_ms':30000}|patch)

    def flash(self):
        b = bytearray(b'\xff' * 0x400000)
        b[0x8000:0x8004] = b'test'
        for i, seq in enumerate((29, 30)):
            struct.pack_into('<I', b, 0x10000+i*4096, seq)
            struct.pack_into('<II', b, 0x10018+i*4096, 2,
                             binascii.crc32(struct.pack('<I', seq), 0xffffffff))
        return b

    def test_inactive_write_preserves_other_boot_record(self):
        b = self.flash(); p = plan(b, b'test')
        self.assertEqual((p['active_slot'], p['destination_slot'], p['application_offset']), (1, 0, 0x20000))
        self.assertEqual(p['metadata_offset'], 0x10000)
        b[p['metadata_offset']:p['metadata_offset']+4096] = p['metadata']
        self.assertEqual(plan(b, b'test')['active_slot'], 0)
        # A torn new metadata record leaves the previous valid slot selected.
        b[0x10000:0x10010] = b'\xff'*16
        self.assertEqual(plan(b, b'test')['active_slot'], 1)

    def test_fail_closed_on_bad_backup_layout_or_pending_trial(self):
        b = self.flash()
        for data, table in [(b[:100], b'test'), (b, b'wrong')]:
            with self.assertRaises(ValueError): plan(data, table)
        struct.pack_into('<I', b, 0x11018, 1)
        with self.assertRaises(ValueError): plan(b, b'test')

    def test_semver_policy(self):
        self.assertTrue(newer('0.2.0', '0.2.0-rc.30'))
        self.assertTrue(newer('0.2.0-rc.10', '0.2.0-rc.9'))
        self.assertFalse(newer('0.2.0+new', '0.2.0+old'))
        self.assertFalse(newer('0.1.9', '0.2.0-rc.1'))
        self.assertFalse(newer('0.2.0-1', '0.2.0-beta'))
        self.assertTrue(newer('0.2.0-beta.1', '0.2.0-beta'))

    def test_signed_size_rejects_truncated_or_forged_segments(self):
        b = bytearray(b'\xff' * 8192); b[:24] = bytes(24); b[0] = 0xe9; b[1] = 1
        struct.pack_into('<II', b, 24, 0x3c000000, 100)
        b[4096:4098] = b'\xe7\x02'
        self.assertEqual(signed_image_size(b), 8192)
        struct.pack_into('<I', b, 28, 0xFFFFFFFF)
        with self.assertRaises(ValueError): signed_image_size(b)
