import importlib.util
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, call, patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("lights_access", ROOT / "lights/tools/access.py")
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)


class LightsAccessTests(unittest.TestCase):
    def run_deferred_provision(self, updates, error=None, acceptance_error=None):
        before = {"compiled": True, "configured": False, "provisioning": False}
        accepted = {"compiled": True, "provisioning": True, "provision_phase": "queued",
                    "provision_generation": 7, "provision_boot": "1234567890abcdef",
                    "provision_deadline_ms": 30000}
        bundle = {"v": 1, "gateway_id": "test-device", "owner_key": "test-owner-key",
                  "guest_password": "test-guest-key", "owner": {"salt": "22" * 16, "verifier": "11" * 384},
                  "guest": {"salt": "33" * 16, "verifier": "11" * 384}}
        original = copy.deepcopy(bundle)
        replies = [before, acceptance_error or accepted, *updates]
        clock = [0.0]
        requests, sleeps, connections = [], [], []
        instance = Mock(profile={"id": "test-device"}, connection=None)

        def request(method, path, *args, **kwargs):
            if instance.connection is None:
                instance.connection = Mock()
                connections.append(instance.connection)
            requests.append((method, path, clock[0]))
            item = replies.pop(0) if replies else accepted
            if isinstance(item, BaseException):
                raise item
            return copy.deepcopy(item)

        def sleep(seconds):
            self.assertIsNone(instance.connection, "TLS retained during an allocation quiet window")
            sleeps.append(seconds)
            clock[0] += seconds

        instance.request.side_effect = request
        with patch.object(access, "Device", return_value=instance), \
                patch.object(access, "protected_read", return_value=bundle), \
                patch.object(access, "verifier", return_value=bytes.fromhex("11" * 384)), \
                patch.object(access.time, "monotonic", side_effect=lambda: clock[0]), \
                patch.object(access.time, "sleep", side_effect=sleep), patch("builtins.print"):
            if error:
                with self.assertRaisesRegex(*error):
                    access.provision(Path("/unused-test-owner-bundle.json"), "127.0.0.1:18443")
            else:
                access.provision(Path("/unused-test-owner-bundle.json"), "127.0.0.1:18443")
        self.assertEqual(bundle, original)
        self.assertIsNone(instance.connection)
        for connection in connections:
            connection.close.assert_called_once_with()
        self.assertEqual(sum(method == "POST" for method, _, _ in requests), 1)
        return requests, sleeps, accepted

    def test_deferred_provision_releases_tls_and_correlates_durable_outcome(self):
        committed = {"provision_boot": "1234567890abcdef", "provision_generation": 7,
                     "provision_phase": "committed", "provision_result": 0,
                     "configured": True, "provisioning": False, "active": False}
        requests, sleeps, _ = self.run_deferred_provision([OSError("peak busy"), committed])
        self.assertEqual([t for method, _, t in requests if method == "GET"], [0.0, 4.0, 9.0])
        self.assertEqual(sleeps, [4, 5])

    def test_deferred_provision_expiry_and_uncertain_storage_never_repost(self):
        for phase, unknown, message in [("expired", False, "expired"),
                                        ("cancelled", False, "cancelled"),
                                        ("failed", True, "storage outcome is unknown")]:
            with self.subTest(phase=phase):
                status = {"provision_boot": "1234567890abcdef", "provision_generation": 7,
                          "provision_phase": phase, "provisioning": False,
                          "provision_outcome_unknown": unknown}
                requests, _, _ = self.run_deferred_provision([status], (RuntimeError, message))
                self.assertEqual(len(requests), 3)

    def test_deferred_provision_reboot_and_replacement_are_unknown_not_success(self):
        for boot, generation in [("0000000000000001", 7), ("1234567890abcdef", 8)]:
            with self.subTest(boot=boot, generation=generation):
                status = {"provision_boot": boot, "provision_generation": generation,
                          "provision_phase": "committed", "configured": True, "provision_result": 0}
                self.run_deferred_provision([status], (RuntimeError, "request changed"))

    def test_deferred_provision_poll_deadline_is_not_renewed(self):
        requests, sleeps, _ = self.run_deferred_provision([], (TimeoutError, "outcome is unknown"))
        self.assertEqual(sum(sleeps), 35)
        self.assertEqual(max(t for _, _, t in requests), 34)

    def test_deferred_provision_lost_acceptance_is_unknown_and_never_reposted(self):
        requests, sleeps, _ = self.run_deferred_provision(
            [], (RuntimeError, "outcome is unknown"), OSError("lost acceptance response"))
        self.assertEqual(len(requests), 2)
        self.assertEqual(sleeps, [])

    def test_native_verifier_preserves_leading_zero_digest_and_salt(self):
        fixtures = json.loads((ROOT / "lights/security/Tests/LightsSecurityTests/Fixtures/idf61.json").read_text())
        fixture = fixtures["leading_zero_verifier_fixture"]
        self.assertTrue(fixture["inner_digest"].startswith("00"))
        self.assertEqual(access.verifier(fixture["username"], fixture["password"],
                                        bytes.fromhex(fixture["salt"])).hex(), fixture["verifier"])

    def test_bundle_creation_is_private_and_cannot_replace_recovery_key(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "device.json"
            profile.write_text(json.dumps({"id": "test-device"}))
            bundle = root / "private" / "access.json"
            with patch.object(access, "PROFILE", profile), patch("builtins.print"):
                access.prepare(bundle)
                original = bundle.read_bytes()
                self.assertEqual(bundle.stat().st_mode & 0o777, 0o600)
                self.assertEqual(bundle.parent.stat().st_mode & 0o777, 0o700)
                with self.assertRaises(FileExistsError):
                    access.prepare(bundle)
            self.assertEqual(bundle.read_bytes(), original)
            self.assertEqual(access.protected_read(bundle)["gateway_id"], "test-device")
            os.chmod(bundle, 0o644)
            with self.assertRaisesRegex(ValueError, "private regular file"):
                access.protected_read(bundle)

    def test_wrong_gateway_or_damaged_verifier_never_authenticates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "device.json"
            profile.write_text(json.dumps({"id": "test-device"}))
            bundle = root / "access.json"
            with patch.object(access, "PROFILE", profile), patch("builtins.print"):
                access.prepare(bundle)
            with patch.object(access, "Device") as device:
                device.return_value.profile = {"id": "other-device"}
                with self.assertRaisesRegex(ValueError, "different gateway"):
                    access.provision(bundle, None)
                device.return_value.login.assert_not_called()
                device.return_value.profile = {"id": "test-device"}
                damaged = json.loads(bundle.read_text())
                damaged["owner"]["verifier"] = "00" * 384
                bundle.write_text(json.dumps(damaged))
                with self.assertRaisesRegex(ValueError, "verifier does not match"):
                    access.provision(bundle, None)
                device.return_value.login.assert_not_called()
                device.return_value.request.assert_not_called()

    def test_quarantine_retry_is_explicit_and_releases_tls_without_waiting(self):
        connection = Mock()
        current = {"compiled": True, "configured": True, "quarantined": False,
                   "quarantine_retry_used": True, "active": False,
                   "resource_blocked": True, "recovery_pending": True}
        with patch.object(access, "Device") as device, patch("builtins.print") as output, \
                patch.object(access.time, "sleep") as sleep, \
                patch.object(access, "protected_read") as read_bundle:
            instance = device.return_value
            instance.connection = connection
            instance.request.side_effect = [{"compiled": True, "quarantined": True}, current]
            access.retry("127.0.0.1:18443")
            device.assert_called_once_with("127.0.0.1:18443", tls12=True)
            instance.login.assert_called_once_with()
            self.assertEqual(instance.request.call_args_list, [
                call("GET", "/api/v1/lights"), call("POST", "/api/v1/lights/retry", {})])
            connection.close.assert_called_once_with()
            self.assertIsNone(instance.connection)
            sleep.assert_not_called()
            read_bundle.assert_not_called()
            self.assertIn("activation still requires available memory", output.call_args_list[0].args[0])

    def test_quarantine_retry_never_posts_when_not_available(self):
        cases = [{"compiled": False}, {"compiled": True, "quarantined": False},
                 {"compiled": True, "quarantined": True, "quarantine_retry_used": True}]
        for before in cases:
            with self.subTest(before=before), patch.object(access, "Device") as device:
                instance = device.return_value
                connection = instance.connection = Mock()
                instance.request.return_value = before
                with self.assertRaises(ValueError):
                    access.retry(None)
                instance.request.assert_called_once_with("GET", "/api/v1/lights")
                connection.close.assert_called_once_with()
                self.assertIsNone(instance.connection)

    def test_quarantine_retry_closes_after_error_and_never_automatically_reposts(self):
        failures = [RuntimeError("lost response"),
                    {"compiled": True, "quarantined": True, "quarantine_retry_used": False}]
        for failure in failures:
            with self.subTest(failure=failure), patch.object(access, "Device") as device:
                instance = device.return_value
                connection = instance.connection = Mock()
                instance.request.side_effect = [{"compiled": True, "quarantined": True}, failure]
                with self.assertRaises(RuntimeError):
                    access.retry(None)
                self.assertEqual(instance.request.call_count, 2)
                connection.close.assert_called_once_with()
                self.assertIsNone(instance.connection)


if __name__ == "__main__":
    unittest.main()
