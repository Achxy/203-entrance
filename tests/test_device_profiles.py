import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import entrance


class DeviceProfileTests(unittest.TestCase):
    def test_separate_boards_preserve_credentials_and_reject_wrong_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(entrance, 'LOCAL', root), patch.object(entrance, 'PROFILE', root/'first.json'):
                first = entrance.create_identity('02:00:00:00:00:01')
                saved = entrance.PROFILE.read_bytes()
                certificate = Path(first['certificate']).read_bytes()
                with self.assertRaises(ValueError): entrance.create_identity('02:00:00:00:00:02')
                with self.assertRaises(ValueError): entrance.create_identity('02:00:00:00:00:02', reset=True)
                self.assertEqual(entrance.PROFILE.read_bytes(), saved)
                entrance.PROFILE = root/'second.json'
                second = entrance.create_identity('02:00:00:00:00:02')
                self.assertEqual((root/'first.json').read_bytes(), saved)
                self.assertEqual(Path(first['certificate']).read_bytes(), certificate)
                for key in ('ssid', 'admin_password', 'ap_password', 'certificate_sha256'):
                    self.assertNotEqual(first[key], second[key])
                self.assertEqual(entrance.create_identity(second['mac']), second)
                self.assertEqual(json.loads(entrance.PROFILE.read_text())['mac'], second['mac'])
                entrance.PROFILE = root/'missing-profile.json'
                with self.assertRaises(ValueError): entrance.create_identity(first['mac'])
                self.assertEqual(Path(first['certificate']).read_bytes(), certificate)

    def test_cli_selects_explicit_device_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            selected = Path(temporary)/'board.json'
            with patch.object(entrance, 'PROFILE', Path(temporary)/'default.json'), \
                 patch.object(entrance, 'cmd_status') as status, \
                 patch.object(sys, 'argv', ['entrance', 'status', '--device-profile', str(selected)]):
                status.side_effect = lambda _: self.assertEqual(entrance.PROFILE, selected.resolve())
                self.assertEqual(entrance.main(), 0)
                status.assert_called_once()


if __name__ == '__main__':
    unittest.main()
