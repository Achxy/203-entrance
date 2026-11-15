"""Local safety boundaries for signing-mode selection; no device or IDF builds."""
from __future__ import annotations

import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_candidate as candidate


class BuildSelectionTests(unittest.TestCase):
    def test_disposable_defaults_never_select_the_production_key(self):
        root = Path("/private/example")
        ordinary = candidate.select_build(root, None)
        disabled = candidate.select_build(root, None, default_off=True)
        self.assertFalse(ordinary.hardware)
        self.assertTrue(ordinary.lighting_enabled)
        self.assertFalse(disabled.lighting_enabled)
        self.assertEqual(ordinary.signing_key.parent, ordinary.output)
        self.assertEqual(disabled.signing_key.parent, disabled.output)
        self.assertNotEqual(ordinary.output, disabled.output)
        self.assertNotEqual(ordinary.signing_key, root / ".local/keys/firmware.pem")

    def test_hardware_requires_an_explicit_safe_prerelease(self):
        root = Path("/private/example")
        for value in (None, "0.4.0", "0.4.0+rc.1", "../escape", "0.4.0-rc.01"):
            with self.subTest(version=value), self.assertRaises(ValueError):
                candidate.select_build(root, value, hardware=True)
        with self.assertRaises(ValueError):
            candidate.select_build(root, "0.4.0-rc.1", hardware=True, default_off=True)
        selected = candidate.select_build(root, "0.3.0-rc.16.lights.1", hardware=True)
        self.assertEqual(selected.output, root / ".local/lights-hardware/0.3.0-rc.16.lights.1")
        self.assertEqual(selected.signing_key, root / ".local/keys/firmware.pem")
        self.assertEqual(selected.public_key, root / "keys/release-public.pem")
        self.assertTrue(selected.hardware)

    def test_semver_comparison_cannot_be_bypassed_by_build_metadata(self):
        cases = [
            ("0.3.0-rc.16", "0.3.0-rc.15", True),
            ("0.3.0-rc.10", "0.3.0-rc.9", True),
            ("0.3.0-rc.15+new", "0.3.0-rc.15+old", False),
            ("0.3.0-lights.99", "0.3.0-rc.15", False),
            ("0.3.0-rc.15.1", "0.3.0-rc.15", True),
            ("0.3.0-rc.15", "0.3.0-rc.15.1", False),
            ("0.3.0-rc.999", "0.3.0", False),
            ("0.3.0", "0.3.0-rc.15", True),
            ("0.3.1-lights.1", "0.3.0", True),
            ("0.3.0-10", "0.3.0-nine", False),
        ]
        for current, baseline, expected in cases:
            with self.subTest(current=current, baseline=baseline):
                self.assertEqual(candidate.version_is_newer(current, baseline), expected)

    def test_profile_selects_only_the_requested_key_without_mutating_baseline(self):
        original = ('CONFIG_SECURE_BOOT_SIGNING_KEY=".local/keys/firmware.pem"\n'
                    'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\nCONFIG_ESP_WIFI_IRAM_OPT=y\n')
        copy = original
        for hardware in (False, True):
            selected = candidate.select_build(Path("/private/example"), "0.4.0-rc.1", hardware=hardware)
            profile = candidate.candidate_config(original, "CONFIG_BT_ENABLED=y\n", selected.signing_key)
            self.assertIn("CONFIG_SECURE_BOOT_SIGNING_KEY=" + json.dumps(str(selected.signing_key)), profile)
            self.assertIn("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y", profile)
            self.assertIn("# CONFIG_ESP_WIFI_IRAM_OPT is not set", profile)
            self.assertEqual(profile.count("CONFIG_SECURE_BOOT_SIGNING_KEY="), 1)
        self.assertEqual(original, copy)

    def test_socket_budget_is_candidate_only(self):
        production = 'CONFIG_LWIP_MAX_SOCKETS=16\nCONFIG_ESP_WIFI_IRAM_OPT=y\n'
        root = Path("/private/example")
        for hardware in (False, True):
            selected = candidate.select_build(root, "0.3.0-rc.16", hardware=hardware)
            profile = candidate.build_config(production, "CONFIG_BT_ENABLED=y\n", selected)
            self.assertIn("CONFIG_LWIP_MAX_SOCKETS=20\n", profile)
            self.assertNotIn("CONFIG_LWIP_MAX_SOCKETS=16\n", profile)
        disabled = candidate.select_build(root, None, default_off=True)
        profile = candidate.build_config(production, "CONFIG_BT_ENABLED=y\n", disabled)
        self.assertIn("CONFIG_LWIP_MAX_SOCKETS=16\n", profile)
        self.assertIn("CONFIG_ESP_WIFI_IRAM_OPT=y\n", profile)
        self.assertNotIn("CONFIG_BT_ENABLED=y", profile)


class ProductionKeyGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.private = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        cls.other = rsa.generate_private_key(public_exponent=65537, key_size=3072)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.selected = candidate.select_build(self.root, "0.4.0-rc.1", hardware=True)
        self.selected.signing_key.parent.mkdir(parents=True, mode=0o700)
        self.selected.signing_key.parent.parent.chmod(0o700)
        self.selected.public_key.parent.mkdir()
        self.selected.signing_key.write_bytes(self.private.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        self.selected.signing_key.chmod(0o600)
        self.write_public(self.private.public_key())

    def write_public(self, public):
        self.selected.public_key.write_bytes(public.public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))

    def test_exact_key_match_is_read_only_and_fingerprinted(self):
        before = candidate.signer_snapshot(self.selected)
        fingerprint = candidate.checked_production_key(self.selected)
        self.assertEqual(len(fingerprint), 64)
        self.assertEqual(candidate.signer_snapshot(self.selected), before)
        self.selected.signing_key.chmod(0o400)
        candidate.checked_production_key(self.selected)
        self.assertEqual(stat.S_IMODE(self.selected.signing_key.stat().st_mode), 0o400)

    def test_wrong_public_key_or_algorithm_is_rejected(self):
        for key in (self.other.public_key(), ec.generate_private_key(ec.SECP256R1()).public_key()):
            self.write_public(key)
            with self.assertRaises(RuntimeError):
                candidate.checked_production_key(self.selected)

    def test_missing_key_is_not_generated(self):
        self.selected.signing_key.unlink()
        with self.assertRaises(FileNotFoundError):
            candidate.checked_production_key(self.selected)
        self.assertFalse(self.selected.signing_key.exists())

    def test_exposed_key_and_parent_modes_are_refused_without_chmod(self):
        self.selected.signing_key.chmod(0o644)
        with self.assertRaises(RuntimeError):
            candidate.checked_production_key(self.selected)
        self.assertEqual(stat.S_IMODE(self.selected.signing_key.stat().st_mode), 0o644)
        self.selected.signing_key.chmod(0o600)
        self.selected.signing_key.parent.chmod(0o755)
        with self.assertRaises(RuntimeError):
            candidate.checked_production_key(self.selected)
        self.assertEqual(stat.S_IMODE(self.selected.signing_key.parent.stat().st_mode), 0o755)

    def test_key_symlink_is_refused(self):
        target = self.root / "secret.pem"
        self.selected.signing_key.rename(target)
        self.selected.signing_key.symlink_to(target)
        with self.assertRaises(RuntimeError):
            candidate.checked_production_key(self.selected)


class OutputAndRevisionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()

    def git(self, *arguments):
        return subprocess.check_output(["git", *arguments], cwd=self.root, text=True,
                                       stderr=subprocess.STDOUT).strip()

    def test_clean_revision_rejects_tracked_and_untracked_changes(self):
        self.git("init", "--quiet")
        for filename in ("keys/release-public.pem", "lights/tools/build_candidate.py"):
            path = self.root / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("test fixture\n")
        self.git("add", ".")
        self.git("-c", "user.name=Build Safety Test", "-c", "user.email=build-test@example.invalid",
                 "commit", "--quiet", "-m", "test fixture")
        revision = candidate.clean_revision(self.root)
        self.assertTrue(revision["clean"])
        self.assertEqual(revision["commit"], self.git("rev-parse", "HEAD"))
        new = self.root / "untracked"
        new.write_text("changed\n")
        with self.assertRaises(RuntimeError):
            candidate.clean_revision(self.root)
        new.unlink()
        (self.root / "keys/release-public.pem").write_text("different key\n")
        self.git("add", "keys/release-public.pem")
        with self.assertRaises(RuntimeError):
            candidate.clean_revision(self.root)

    def test_hardware_output_is_private_and_rejects_symlinks(self):
        output = self.root / ".local/lights-hardware/0.4.0-rc.1"
        candidate.prepare_private_output(self.root, output)
        for path in (output, output.parent, output.parent.parent):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700)
        (output.parent / "redirect").symlink_to(output, target_is_directory=True)
        with self.assertRaises(RuntimeError):
            candidate.prepare_private_output(self.root, output.parent / "redirect")
        output.chmod(0o755)
        with self.assertRaises(RuntimeError):
            candidate.prepare_private_output(self.root, output)

    def test_a_hardware_version_cannot_be_reused_for_different_sources(self):
        revision = {"commit": "a" * 40}
        value = {"mode": "hardware-qualification", "metadata": {"version": "0.4.0-rc.1"},
                 "revision": revision, "source_sha256": "original", "configuration_input_sha256": "settings"}
        (self.root / "summary.json").write_text(json.dumps(value))
        expected = {"source_sha256": "original", "configuration_input_sha256": "settings"}
        candidate.check_hardware_output(self.root, "0.4.0-rc.1", revision, expected)
        with self.assertRaises(RuntimeError):
            candidate.check_hardware_output(self.root, "0.4.0-rc.1", revision, expected | {"source_sha256": "modified"})
        with self.assertRaises(RuntimeError):
            candidate.check_hardware_output(self.root, "0.4.0-rc.1", {"commit": "b" * 40}, expected)
        with self.assertRaises(RuntimeError):
            candidate.check_hardware_output(self.root, "0.4.0-rc.1", revision, expected | {"configuration_input_sha256": "modified"})

    def test_build_subdirectory_cannot_redirect_to_the_baseline(self):
        output = self.root / "candidate"
        baseline = self.root / "production-build"
        output.mkdir()
        baseline.mkdir()
        marker = baseline / "retained.bin"
        marker.write_bytes(b"retained production artifact")
        (output / "build").symlink_to(baseline, target_is_directory=True)
        with self.assertRaises(RuntimeError):
            candidate.check_build_directory(output)
        self.assertEqual(marker.read_bytes(), b"retained production artifact")


if __name__ == "__main__":
    unittest.main()
