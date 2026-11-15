import importlib.util
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
