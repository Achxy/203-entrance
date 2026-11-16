#!/usr/bin/env python3
"""Owner-only lighting bootstrap through the existing pinned management API.

Never changes Wi-Fi, resets a bulb, flashes firmware, or prints a credential.
The gateway stores SRP verifiers; the recoverable owner bundle stays local.
"""
from __future__ import annotations
import argparse
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import secrets
import stat
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from entrance import APIError, Device, PROFILE, SDK  # noqa: E402
from firmware import IDF_COMMIT  # noqa: E402

DEFAULT_BUNDLE = ROOT / ".local/lights/owner-access.json"


def verifier(username: str, password: str, salt: bytes) -> bytes:
    if username not in ("203-owner", "203-guest") or len(salt) != 16:
        raise ValueError("Invalid SRP profile")
    if not 12 <= len(password.encode("utf-8")) <= 256:
        raise ValueError("Password must contain 12–256 UTF-8 bytes")
    actual = subprocess.check_output(["git", "-C", str(SDK), "rev-parse", "HEAD"], text=True).strip()
    if actual != IDF_COMMIT:
        raise ValueError("Pinned ESP-IDF revision does not match")
    source = SDK / "examples/protocols/esp_local_ctrl/scripts/security/srp6a.py"
    sys.path.insert(0, str(source.parent.parent))
    spec = importlib.util.spec_from_file_location("idf_srp_parameters", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    modulus, generator = module.get_ng(module.NG_3072)
    inner = hashlib.sha512((username + ":" + password).encode("utf-8")).digest()
    x = int.from_bytes(hashlib.sha512(salt + inner).digest(), "big")
    return pow(generator, x, modulus).to_bytes(384, "big")


def profile_for(username: str, password: str) -> dict[str, str]:
    salt = secrets.token_bytes(16)
    return {"salt": salt.hex(), "verifier": verifier(username, password, salt).hex()}


def protected_read(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
            raise ValueError("Access bundle must be a private regular file (chmod 600)")
        if info.st_uid != os.getuid() or info.st_size > 8192:
            raise ValueError("Access bundle ownership or size is invalid")
        return json.load(stream)


def prepare(path: Path) -> None:
    device = json.loads(PROFILE.read_text())
    owner, guest = secrets.token_urlsafe(32), secrets.token_urlsafe(20)
    bundle = {"v": 1, "gateway_id": device["id"], "owner_key": owner,
              "guest_password": guest, "owner": profile_for("203-owner", owner),
              "guest": profile_for("203-guest", guest)}
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as stream:
        json.dump(bundle, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    print(f"Owner recovery bundle prepared: {path}")
    print("No device was contacted. Keep this file private and back it up securely.")


def provision(path: Path, host: str | None) -> None:
    bundle = protected_read(path)
    device = Device(host, tls12=True)
    if bundle.get("v") != 1 or bundle.get("gateway_id") != device.profile["id"]:
        raise ValueError("Access bundle belongs to a different gateway")
    for role, secret in (("owner", "owner_key"), ("guest", "guest_password")):
        entry = bundle[role]
        expected = verifier("203-" + role, bundle[secret], bytes.fromhex(entry["salt"]))
        if not secrets.compare_digest(expected.hex(), entry["verifier"]):
            raise ValueError("Access bundle verifier does not match its credential")
    try:
        device.login()
        before = device.request("GET", "/api/v1/lights")
        if not before.get("compiled"):
            raise ValueError("The installed firmware does not contain the lighting gateway")
        if before.get("configured") or before.get("provisioning"):
            raise ValueError("Lighting is already configured or provisioning; it was not overwritten")
        if before.get("quarantined"):
            raise ValueError("Lighting is quarantined after an abnormal reset; inspect status before an explicit retry")
        body = {f"{role}_{name}": bundle[role][name]
                for role in ("owner", "guest") for name in ("salt", "verifier")}
        try:
            accepted = device.request("POST", "/api/v1/lights/provision", body)
        except APIError:
            raise
        except Exception as error:
            raise RuntimeError("Provisioning outcome is unknown; check status before any retry") from error
        finally:
            close_connection(device)
        generation, boot = accepted.get("provision_generation"), accepted.get("provision_boot")
        if (type(generation) is not int or not 0 < generation <= 0xffffffff or
                not isinstance(boot, str) or len(boot) != 16 or
                any(c not in "0123456789abcdef" for c in boot)):
            raise RuntimeError("Provisioning acceptance cannot be correlated; check status before any retry")
        # The server's fixed30s admission deadline does not extend. Allow5s
        # solely for observing a transaction already admitted before that limit.
        deadline = time.monotonic() + 35
        time.sleep(4)
        while time.monotonic() < deadline:
            current = None
            try:
                current = device.request("GET", "/api/v1/lights",
                                         timeout=min(5, max(0.1, deadline - time.monotonic())))
            except (OSError, http.client.HTTPException):
                # Read-only polling may overlap a guarded allocation peak.
                # Keep the original deadline and never replay the POST.
                pass
            finally:
                close_connection(device)
            if current is None:
                time.sleep(min(5, max(0, deadline - time.monotonic())))
                continue
            if current.get("provision_boot") != boot or current.get("provision_generation") != generation:
                raise RuntimeError("Gateway boot or provisioning request changed; check status before any retry")
            if current.get("provision_outcome_unknown"):
                raise RuntimeError("Provisioning storage outcome is unknown; do not overwrite or automatically retry")
            phase = current.get("provision_phase")
            if phase == "committed" and current.get("provision_result") == 0 and current.get("configured"):
                print("Gateway credential record committed. Physical bulb and coexistence tests remain required.")
                print(json.dumps(current, indent=2))
                return
            if phase in ("failed", "expired", "cancelled") or not current.get("provisioning"):
                raise RuntimeError(f"Gateway provisioning {phase or 'failed'}; inspect its resource/storage status")
            time.sleep(min(5, max(0, deadline - time.monotonic())))
        raise TimeoutError("Provisioning outcome is unknown; check status before any retry")
    finally:
        close_connection(device)


def close_connection(device: Device) -> None:
    if device.connection is not None:
        device.connection.close()
        device.connection = None


def retry(host: str | None) -> None:
    """Release this boot's crash quarantine; never change an access bundle."""
    device = Device(host, tls12=True)
    try:
        device.login()
        before = device.request("GET", "/api/v1/lights")
        if not before.get("compiled"):
            raise ValueError("The installed firmware does not contain the lighting gateway")
        if not before.get("quarantined") or before.get("quarantine_retry_used"):
            raise ValueError("No lighting quarantine retry is available for this boot")
        current = device.request("POST", "/api/v1/lights/retry", {})
        if current.get("quarantined") or not current.get("quarantine_retry_used"):
            raise RuntimeError("Lighting retry was not accepted; inspect status before any retry")
        print("Lighting quarantine released for this boot; activation still requires available memory.")
        print("Saved credentials and light settings were not changed.")
        print(json.dumps(current, indent=2))
    finally:
        # Leave the allocation gate a quiet window after an accepted retry. The
        # existing bounded recovery poll can activate after this TLS session ends.
        close_connection(device)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "status", "provision", "retry"))
    parser.add_argument("--bundle", type=Path, default=DEFAULT_BUNDLE)
    parser.add_argument("--host", help="Pinned device endpoint, including a local USB hub proxy port")
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.bundle.resolve())
    elif args.command == "provision":
        provision(args.bundle.resolve(), args.host)
    elif args.command == "retry":
        retry(args.host)
    else:
        device = Device(args.host, tls12=True)
        device.login()
        print(json.dumps(device.request("GET", "/api/v1/lights"), indent=2))


if __name__ == "__main__":
    main()
