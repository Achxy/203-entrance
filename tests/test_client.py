import http.client
import hashlib
import json
import ssl
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from entrance import APIError, Device


class UploadRecoveryTests(unittest.TestCase):
    def device(self, replies):
        device = Device.__new__(Device)
        device.recovered_chunks = 0
        device.request = Mock(side_effect=replies)
        self.addCleanup(patch.stopall)
        patch("entrance.time.sleep").start()
        return device

    def test_lost_reply_after_commit_is_not_replayed(self):
        device = self.device([TimeoutError(), {"phase": "receiving", "received": 8192}])
        device.write_chunk("ticket", 4096, b"x" * 4096)
        self.assertEqual([call.args[0] for call in device.request.call_args_list], ["PUT", "GET"])
        self.assertEqual(device.recovered_chunks, 1)

    def test_lost_request_retried_only_after_unchanged_offset(self):
        device = self.device([http.client.RemoteDisconnected(), {"phase": "receiving", "received": 4096}, {"ok": True}])
        device.write_chunk("ticket", 4096, b"x" * 4096)
        calls = device.request.call_args_list
        self.assertEqual([call.args[0] for call in calls], ["PUT", "GET", "PUT"])
        self.assertEqual(calls[0], calls[2])
        self.assertEqual(device.recovered_chunks, 1)

    def test_body_timeout_recovers_but_authentication_does_not(self):
        device = self.device([APIError(408, {"error": "ESP_ERR_TIMEOUT"}), {"phase": "receiving", "received": 0}, {}])
        device.write_chunk("ticket", 0, b"data")
        device = self.device([APIError(401, {"error": "authentication"})])
        with self.assertRaises(APIError):
            device.write_chunk("ticket", 0, b"data")
        self.assertEqual(device.request.call_count, 1)

    def test_unknown_outcome_never_blindly_replayed(self):
        device = self.device([TimeoutError()] * 4)
        with self.assertRaisesRegex(TimeoutError, "Cannot resolve"):
            device.write_chunk("ticket", 0, b"data")
        self.assertEqual([call.args[0] for call in device.request.call_args_list], ["PUT", "GET", "GET", "GET"])

    def test_retries_are_bounded(self):
        device = self.device([TimeoutError(), {"phase": "receiving", "received": 0}] * 3)
        with self.assertRaisesRegex(TimeoutError, "three attempts"):
            device.write_chunk("ticket", 0, b"data")
        self.assertEqual(device.request.call_count, 6)

    def test_divergent_offset_and_failed_transaction_stop(self):
        for state in [{"phase": "receiving", "received": 2}, {"phase": "failed", "received": 4, "detail": "expired"}]:
            with self.subTest(state=state):
                device = self.device([TimeoutError(), state])
                with self.assertRaises(ValueError):
                    device.write_chunk("ticket", 0, b"data")
                self.assertEqual(device.request.call_count, 2)


class TLSPinningTests(unittest.TestCase):
    def test_smaller_handshake_retains_ca_and_pin_checks(self):
        certificate = b'test-device-certificate'
        for tls12 in (False, True):
            for matching in (False, True):
                with self.subTest(tls12=tls12, matching=matching):
                    profile = {'host': '127.0.0.1:18443', 'certificate': 'device.pem',
                               'certificate_sha256': hashlib.sha256(certificate).hexdigest()
                               if matching else '0' * 64}
                    context = Mock(maximum_version=ssl.TLSVersion.MAXIMUM_SUPPORTED)
                    connection = Mock()
                    connection.sock.getpeercert.return_value = certificate
                    response = connection.getresponse.return_value
                    response.status = 200
                    response.read.return_value = b'{"ok":true}'
                    with patch('entrance.PROFILE', Mock(read_text=lambda: json.dumps(profile))), \
                         patch('entrance.ssl.create_default_context', return_value=context) as factory, \
                         patch('entrance.http.client.HTTPSConnection', return_value=connection):
                        device = Device(tls12=tls12)
                        if matching:
                            self.assertEqual(device.request('GET', '/status'), {'ok': True})
                        else:
                            with self.assertRaisesRegex(ValueError, 'certificate pin changed'):
                                device.request('POST', '/session', {'password': 'never-sent'})
                            connection.request.assert_not_called()
                            connection.close.assert_called_once()
                        factory.assert_called_once_with(cafile='device.pem')
                        self.assertEqual(context.maximum_version, ssl.TLSVersion.TLSv1_2
                                         if tls12 else ssl.TLSVersion.MAXIMUM_SUPPORTED)


if __name__ == "__main__":
    unittest.main()
