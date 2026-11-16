import http.client
import hashlib
import io
import json
import ssl
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from entrance import APIError, Device, cmd_upload


class UploadClockTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.sleeps = []
        for name, value in [("monotonic", lambda: self.now), ("sleep", self.sleep)]:
            change = patch("entrance.time." + name, value)
            change.start()
            self.addCleanup(change.stop)

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds

    def device(self, replies):
        device = Device.__new__(Device)
        device.recovered_chunks = 0
        device._upload_started = None
        device._upload_progress_started = None
        device.login = Mock()
        device.request_times = []
        replies = iter(replies)

        def request(method, path, *args, **kwargs):
            device.request_times.append(self.now)
            reply = next(replies)
            if isinstance(reply, tuple):
                delay, reply = reply
                self.now += kwargs.get("timeout", 20) if delay is None else delay
            if isinstance(reply, BaseException):
                raise reply
            return reply

        device.request = Mock(side_effect=request)
        return device


class UploadRecoveryTests(UploadClockTests):
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
        self.assertEqual(self.sleeps, [0.5, 1.0])

    def test_divergent_offset_and_failed_transaction_stop(self):
        for state in [{"phase": "receiving", "received": 2}, {"phase": "failed", "received": 4, "detail": "expired"}]:
            with self.subTest(state=state):
                device = self.device([TimeoutError(), state])
                with self.assertRaises(ValueError):
                    device.write_chunk("ticket", 0, b"data")
                self.assertEqual(device.request.call_count, 2)

    def test_full_socket_waits_resolve_in_eighteen_not_thirty_six_seconds(self):
        device = self.device([(None, TimeoutError())] * 4)
        with self.assertRaisesRegex(TimeoutError, "Cannot resolve"):
            device.write_chunk("ticket", 57344, b"x" * 8192)
        calls = device.request.call_args_list
        self.assertEqual([call.args[0] for call in calls], ["PUT", "GET", "GET", "GET"])
        self.assertEqual([call.kwargs["timeout"] for call in calls], [8, 3, 3, 3])
        self.assertEqual(device.request_times, [0, 8, 11.5, 15])
        self.assertEqual(self.now, 18)

    def test_status_reads_do_not_restart_recovery_budget(self):
        state = {"phase": "receiving", "received": 0}
        device = self.device([(8, TimeoutError()), (3, state)] * 2)
        with self.assertRaisesRegex(TimeoutError, "budget"):
            device.write_chunk("ticket", 0, b"data")
        self.assertEqual([call.args[0] for call in device.request.call_args_list], ["PUT", "GET", "PUT", "GET"])
        self.assertEqual(self.now, 22.5)
        self.assertEqual(self.sleeps, [0.5])

    def test_retry_backoff_must_leave_time_for_put_and_reconciliation(self):
        state = {"phase": "receiving", "received": 0}
        device = self.device([(7, TimeoutError()), (1, state), (3, TimeoutError()), (2, state)])
        with self.assertRaisesRegex(TimeoutError, "budget"):
            device.write_chunk("ticket", 0, b"data")
        # 11.5 seconds remain: the next 1s backoff plus8s PUT/3s GET cannot fit.
        self.assertEqual(self.now, 13.5)
        self.assertEqual(self.sleeps, [0.5])
        self.assertEqual(device.request.call_count, 4)

    def test_delayed_ack_uses_put_start_for_next_chunk(self):
        for replies, count in [([(15, {"ok": True})], 1),
                               ([(8, TimeoutError()), (7, {"phase": "receiving", "received": 4})], 2)]:
            with self.subTest(resolved_by_status=count == 2):
                self.now = 0
                device = self.device(replies)
                device.write_chunk("ticket", 0, b"data")
                with self.assertRaisesRegex(TimeoutError, "budget"):
                    device.write_chunk("ticket", 4, b"next")
                self.assertEqual(device.request.call_count, count)
                self.assertEqual(self.now, 15)

    def test_retry_success_anchors_progress_to_that_put_start(self):
        device = self.device([(1, TimeoutError()), (1, {"phase": "receiving", "received": 0}),
                              (1, {"ok": True}), {"ok": True}])
        device.write_chunk("ticket", 0, b"data")
        self.now = 16
        device.write_chunk("ticket", 4, b"next")
        self.assertEqual(device.request_times, [0, 1, 2.5, 16])
        self.assertEqual(device.recovered_chunks, 1)

    def test_late_put_or_status_ack_is_rejected_without_another_chunk(self):
        cases = [[(25, {"ok": True})],
                 [(8, TimeoutError()), (17, {"phase": "receiving", "received": 4})]]
        for replies in cases:
            with self.subTest(status=len(replies) == 2):
                self.now = 0
                device = self.device(replies)
                with self.assertRaisesRegex(TimeoutError, "budget"):
                    device.write_chunk("ticket", 0, b"data")
                with self.assertRaisesRegex(TimeoutError, "budget"):
                    device.write_chunk("ticket", 4, b"next")
                self.assertEqual(device.request.call_count, len(replies))
                self.assertEqual(device.recovered_chunks, 0)

    def test_request_overrun_is_not_a_hard_deadline_but_stops_followup_io(self):
        device = self.device([(31, TimeoutError())])
        with self.assertRaisesRegex(TimeoutError, "budget"):
            device.write_chunk("ticket", 0, b"data")
        self.assertEqual(self.now, 31)
        self.assertEqual(device.request.call_count, 1)

    def test_sleep_overrun_stops_both_status_and_put_retries(self):
        for reply in [TimeoutError(), {"phase": "receiving", "received": 0}]:
            with self.subTest(status_retry=isinstance(reply, Exception)):
                self.now = 0
                device = self.device([TimeoutError(), reply])
                with patch("entrance.time.sleep", lambda seconds: self.sleep(seconds + 30)):
                    with self.assertRaisesRegex(TimeoutError, "budget"):
                        device.write_chunk("ticket", 0, b"data")
                self.assertEqual(device.request.call_count, 2)

    def test_total_upload_budget_does_not_reset_on_chunk_progress(self):
        device = self.device([{}] * 29 + [(8, TimeoutError()), (3, {"phase": "receiving", "received": 116})])
        device.begin_upload_budget()
        for index in range(29):
            self.now = index * 10
            device.write_chunk("ticket", index * 4, b"data")
        self.now = 281
        with self.assertRaisesRegex(TimeoutError, "budget"):
            device.write_chunk("ticket", 116, b"data")
        self.assertEqual(self.now, 292)
        self.assertEqual(device.request.call_count, 31)
        self.assertEqual(self.sleeps, [])

    def test_authentication_pin_and_certificate_errors_are_terminal(self):
        failures = [APIError(401, {"error": "authentication"}),
                    APIError(403, {"error": "ticket"}),
                    APIError(401, {"error": "ESP_ERR_TIMEOUT"}),
                    APIError(403, {"error": "ESP_ERR_TIMEOUT"}),
                    ValueError("Device certificate pin changed"),
                    ssl.SSLCertVerificationError("synthetic certificate rejection")]
        for failure in failures:
            for after_put in (False, True):
                with self.subTest(error=type(failure).__name__, status_read=after_put):
                    replies = [TimeoutError(), failure] if after_put else [failure]
                    device = self.device(replies)
                    with self.assertRaises(type(failure)):
                        device.write_chunk("ticket", 0, b"data")
                    self.assertEqual(device.request.call_count, len(replies))


class UploadCommandBudgetTests(UploadClockTests):
    def upload(self, device, stream=None):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "synthetic.bin"
            image.write_bytes(b"data")
            args = SimpleNamespace(image=str(image), host=None, tls12=False, no_confirm=True)
            metadata = {"size": 4, "sha256": "0" * 64, "version": "test"}
            with patch("entrance.Device", return_value=device), \
                 patch("entrance.image_metadata", return_value=metadata), \
                 patch("entrance.tool"), patch("builtins.print"):
                if stream is None:
                    cmd_upload(args)
                else:
                    with patch("entrance.Path.open", return_value=stream):
                        cmd_upload(args)

    def test_first_chunk_budget_starts_before_start_request(self):
        device = self.device([(15, {"ticket": "ticket", "chunk_size": 4}), {}])
        with self.assertRaisesRegex(TimeoutError, "budget"):
            self.upload(device)
        self.assertEqual([call.args[1] for call in device.request.call_args_list],
                         ["/api/v1/ota/start", "/api/v1/ota/abort"])

    def test_upload_start_reply_after_budget_never_writes_a_chunk(self):
        device = self.device([(296, {"ticket": "ticket", "chunk_size": 4}), {}])
        with self.assertRaisesRegex(TimeoutError, "budget"):
            self.upload(device)
        self.assertEqual([call.args[1] for call in device.request.call_args_list],
                         ["/api/v1/ota/start", "/api/v1/ota/abort"])

    def test_expiry_after_last_chunk_prevents_finish(self):
        clock = self

        class DelayedEOF(io.BytesIO):
            def read(self, size=-1):
                value = super().read(size)
                if not value:
                    clock.now = 295
                return value

        device = self.device([{"ticket": "ticket", "chunk_size": 4}, {}, {}])
        with self.assertRaisesRegex(TimeoutError, "budget"):
            self.upload(device, DelayedEOF(b"data"))
        self.assertEqual([call.args[1] for call in device.request.call_args_list],
                         ["/api/v1/ota/start", "/api/v1/ota/chunk", "/api/v1/ota/abort"])

    def test_timely_transfer_still_finishes(self):
        device = self.device([{"ticket": "ticket", "chunk_size": 4}, (1, {}), {}])
        self.upload(device)
        self.assertEqual([call.args[1] for call in device.request.call_args_list],
                         ["/api/v1/ota/start", "/api/v1/ota/chunk", "/api/v1/ota/finish"])

    def test_certificate_failure_at_finish_is_terminal(self):
        device = self.device([{"ticket": "ticket", "chunk_size": 4}, {},
                              ssl.SSLCertVerificationError("synthetic rejection"), {}])
        with self.assertRaises(ssl.SSLCertVerificationError):
            self.upload(device)
        self.assertEqual(device.request.call_args_list[-1].args[1], "/api/v1/ota/abort")


class TLSPinningTests(unittest.TestCase):
    def test_connection_failure_is_explicitly_closed_before_retry(self):
        connection = Mock()
        connection.connect.side_effect = ssl.SSLCertVerificationError("synthetic rejection")
        profile = {"host": "127.0.0.1:18443", "certificate": "synthetic.pem"}
        with patch("entrance.PROFILE", Mock(read_text=lambda: json.dumps(profile))), \
             patch("entrance.ssl.create_default_context", return_value=Mock()), \
             patch("entrance.http.client.HTTPSConnection", return_value=connection):
            device = Device()
            with self.assertRaises(ssl.SSLCertVerificationError):
                device.request("POST", "/session", {"password": "never-sent"})
            connection.close.assert_called_once()
            connection.request.assert_not_called()
            self.assertIsNone(device.connection)

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
