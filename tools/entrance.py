#!/usr/bin/env python3
"""Build, provision, deploy and recover 203 Entrance firmware."""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import getpass
import hashlib
import http.client
import ipaddress
import json
import os
import re
import secrets
import shlex
import shutil
import ssl
import subprocess
import sys
import time
from pathlib import Path

from firmware import (BOARD, IDF_COMMIT, RELEASE_REPO, SLOT_BYTES, image_metadata,
                      manifest_bytes, sign_manifest, validate_version, verify_manifest)

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / ".local"
SDK = ROOT / ".tools/esp-idf"
TOOLS = ROOT / ".tools/idf-tools"
KEY = LOCAL / "keys/firmware.pem"
PUBLIC_KEY = LOCAL / "keys/firmware-public.pem"
PROFILE = LOCAL / "device.json"


def run(args, *, capture=False, env=None, **kwargs):
    return subprocess.run([str(x) for x in args], cwd=ROOT, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None, env=env, **kwargs)


def private_write(path: Path, data: str | bytes):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
    fd = os.open(path, flags, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data.encode() if isinstance(data, str) else data)
    path.chmod(0o600)


def runtime() -> Path:
    choices = sorted((TOOLS / "python_env").glob("idf6.1_py3.12_env/bin/python"))
    return choices[0] if choices else Path(sys.executable)


def idf_env() -> dict:
    env = os.environ.copy()
    env.update(IDF_PATH=str(SDK), IDF_TOOLS_PATH=str(TOOLS), ESP_PYTHON=str(runtime()))
    env["PATH"] = str(runtime().parent) + os.pathsep + env["PATH"]
    result = run(["bash", "-c", 'set -e; source "$1/export.sh" >/dev/null; env -0', "entrance", SDK],
                 capture=True, env=env)
    return dict(item.split("=", 1) for item in result.stdout.split("\0") if "=" in item)


def idf(*args):
    return run([runtime(), SDK / "tools/idf.py", *args], env=idf_env())


def tool(module: str, *args, capture=False):
    if module == "esptool" and "--port" in args:
        port = Path(args[args.index("--port") + 1])
        deadline = time.monotonic() + 15
        # USB Serial/JTAG briefly disappears while the C3 resets/re-enumerates.
        time.sleep(2)
        while not port.exists() and time.monotonic() < deadline:
            time.sleep(0.25)
    return run([runtime(), "-m", module, *args], capture=capture)


def cmd_setup(args):
    (ROOT / ".tools").mkdir(exist_ok=True)
    LOCAL.mkdir(exist_ok=True, mode=0o700)
    if not SDK.exists():
        run(["git", "clone", "--branch", "v6.1", "--depth", "1", "--recurse-submodules",
             "--shallow-submodules", "https://github.com/espressif/esp-idf.git", SDK])
    actual = run(["git", "-C", SDK, "rev-parse", "HEAD"], capture=True).stdout.strip()
    if actual != IDF_COMMIT:
        raise ValueError(f"SDK revision mismatch: expected {IDF_COMMIT}, found {actual}")
    if sys.version_info[:2] != (3, 12):
        raise ValueError("Run setup using Python 3.12. Install it from python.org or your package manager first.")
    env = os.environ.copy()
    env["IDF_TOOLS_PATH"] = str(TOOLS)
    run([sys.executable, SDK / "tools/idf_tools.py", "install", "--targets=esp32c3"], env=env)
    run([sys.executable, SDK / "tools/idf_tools.py", "install-python-env"], env=env)
    run([runtime(), "-m", "pip", "install", "-r", ROOT / "tools/requirements-tools.txt"])
    run([runtime(), ROOT / "tools/entrance.py", "keygen"])
    print("Toolchain ready. Run ./entrance doctor and ./entrance build.")


def cmd_keygen(args):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    if KEY.exists():
        key = serialization.load_pem_private_key(KEY.read_bytes(), password=None)
    else:
        if PROFILE.exists():
            raise ValueError("A device is already provisioned but its signing key is missing. Restore the key backup; refusing automatic key replacement.")
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        private_write(KEY, key.private_bytes(serialization.Encoding.PEM,
                                            serialization.PrivateFormat.PKCS8,
                                            serialization.NoEncryption()))
    if key.key_size != 3072:
        raise ValueError("Existing signing key is not RSA-3072; refusing to replace it")
    public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    if PROFILE.exists():
        expected = json.loads(PROFILE.read_text()).get("signing_public_sha256")
        if expected and expected != hashlib.sha256(public).hexdigest():
            raise ValueError("Signing key fingerprint differs from the provisioned device; use the documented USB reprovisioning procedure.")
    private_write(PUBLIC_KEY, public)
    print("Signing public-key SHA-256:", hashlib.sha256(public).hexdigest())
    print("Private key retained at", KEY, "— keep an offline backup.")


def cmd_doctor(args):
    checks = {"python": sys.version.split()[0], "sdk_present": SDK.exists(),
              "managed_python": str(runtime()), "signing_key_present": KEY.exists(),
              "device_profile_present": PROFILE.exists(), "source": "Achxy/203-entrance",
              "releases": RELEASE_REPO}
    if SDK.exists():
        checks["sdk_commit"] = run(["git", "-C", SDK, "rev-parse", "HEAD"], capture=True).stdout.strip()
    try:
        from serial.tools.list_ports import comports
        checks["usb_devices"] = [{"port": p.device, "serial": p.serial_number} for p in comports() if p.vid == 0x303A]
    except ImportError:
        checks["usb_devices"] = "Run setup to install pyserial"
    print(json.dumps(checks, indent=2))


def cmd_build(args):
    if not KEY.exists():
        raise ValueError("Signing key missing. Run ./entrance keygen; never regenerate a deployed device’s key casually.")
    version = validate_version(args.version)
    actual = run(["git", "-C", SDK, "rev-parse", "HEAD"], capture=True).stdout.strip()
    if actual != IDF_COMMIT:
        raise ValueError("SDK revision does not match toolchain.json")
    inputs = source_fingerprint()
    fault = {"none": 0, "startup": 1, "watchdog": 2, "reset": 3, "activation": 4}[args.fault]
    idf("-D", "IDF_TARGET=esp32c3", "-D", f"PROJECT_VER={version}",
        "-D", f"ENTRANCE_TEST_FAULT={fault}", "build")
    binary = ROOT / "build/203-entrance.bin"
    metadata = image_metadata(binary)
    tool("espsecure", "verify-signature", "--version", "2", "--keyfile", PUBLIC_KEY, binary)
    if metadata["version"] != version:
        raise ValueError("Build version does not match requested version")
    cmd_audit(args)
    if inputs != source_fingerprint():
        raise ValueError("Source changed during the build; rebuild before deploying")
    provenance = {"commit": run(["git", "rev-parse", "HEAD"], capture=True).stdout.strip(),
                  "dirty": bool(run(["git", "status", "--porcelain"], capture=True).stdout.strip()),
                  "inputs_sha256": inputs, "firmware_sha256": metadata["sha256"],
                  "version": version, "idf": actual, "fault": args.fault}
    private_write(LOCAL / "build-provenance.json", json.dumps(provenance, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))
    print("Slot headroom:", SLOT_BYTES - metadata["size"], "bytes")


def source_fingerprint():
    files = run(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], capture=True).stdout
    digest = hashlib.sha256()
    for name in sorted(set(files.split("\0")) - {""}):
        path = ROOT / name
        digest.update(name.encode() + b"\0")
        digest.update(path.read_bytes() if path.is_file() else b"<deleted>")
        digest.update(b"\0")
    return digest.hexdigest()


def serial_port(args) -> str:
    if getattr(args, "port", None):
        return args.port
    from serial.tools.list_ports import comports
    ports = [p.device for p in comports() if p.vid == 0x303A]
    if len(ports) != 1:
        raise ValueError("Specify --port: expected exactly one attached Espressif USB board")
    return ports[0]


def inspect_board(port: str) -> dict:
    output = tool("esptool", "--chip", "esp32c3", "--port", port, "get-security-info", capture=True).stdout
    if "Secure Boot: Disabled" not in output or "Flash Encryption: Disabled" not in output:
        raise ValueError("Board has unexpected security settings; refusing this provisioning flow")
    mac = re.search(r"MAC:\s+([0-9a-fA-F:]{17})", output)
    flash = tool("esptool", "--chip", "esp32c3", "--port", port, "flash-id", capture=True).stdout
    if not mac or "Detected flash size: 4MB" not in flash:
        raise ValueError("Expected the planned 4 MB ESP32-C3 hardware")
    return {"mac": mac[1].lower(), "security": output, "flash": flash}


def cmd_backup(args):
    port = serial_port(args)
    board = inspect_board(port)
    directory = LOCAL / "backups" / dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    directory.mkdir(parents=True, mode=0o700)
    path = directory / "flash.bin"
    tool("esptool", "--chip", "esp32c3", "--port", port, "--baud", "460800", "read-flash", "0", "0x400000", path)
    path.chmod(0o600)
    data = path.read_bytes()
    if len(data) != 0x400000:
        raise ValueError("Backup is incomplete")
    board.update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
    private_write(directory / "metadata.json", json.dumps(board, indent=2) + "\n")
    print("Verified backup saved:", directory)


def create_identity(mac: str, reset=False) -> dict:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID
    if PROFILE.exists() and not reset:
        profile = json.loads(PROFILE.read_text())
        if profile["mac"] != mac:
            raise ValueError("Profile belongs to a different board")
        return profile
    if PROFILE.exists():
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        old = json.loads(PROFILE.read_text())
        backup = LOCAL / "access-backups" / stamp
        private_write(backup / "device.json", PROFILE.read_bytes())
        for name in ("certificate", "tls_key"):
            private_write(backup / Path(old[name]).name, Path(old[name]).read_bytes())
    device = "entrance-" + mac.replace(":", "")[-6:]
    directory = LOCAL / "devices" / device
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, device)])
    now = dt.datetime.now(dt.timezone.utc)
    certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                   .public_key(key.public_key()).serial_number(x509.random_serial_number())
                   .not_valid_before(now - dt.timedelta(days=1)).not_valid_after(now + dt.timedelta(days=3650))
                   .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address("192.168.4.1")),
                                                              x509.DNSName(device + ".local")]), critical=False)
                   .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
                   .sign(key, hashes.SHA256()))
    private_write(directory / "device-cert.pem", certificate.public_bytes(serialization.Encoding.PEM))
    private_write(directory / "device-key.pem", key.private_bytes(serialization.Encoding.PEM,
                  serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    password = secrets.token_urlsafe(24)
    salt = secrets.token_bytes(16)
    profile = {"mac": mac, "id": device, "ssid": "203-" + mac.replace(":", "")[-6:].upper(),
               "ap_password": secrets.token_urlsafe(15), "admin_password": password,
               "admin_salt": salt.hex(), "admin_hash": hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 100000).hex(),
               "certificate": str(directory / "device-cert.pem"), "tls_key": str(directory / "device-key.pem"),
               "certificate_sha256": certificate.fingerprint(hashes.SHA256()).hex(), "host": "192.168.4.1"}
    private_write(PROFILE, json.dumps(profile, indent=2) + "\n")
    return profile


def identity_image(profile: dict) -> Path:
    from cryptography.hazmat.primitives import serialization
    public = serialization.load_pem_public_key(PUBLIC_KEY.read_bytes())
    fingerprint = hashlib.sha256(PUBLIC_KEY.read_bytes()).hexdigest()
    if profile.get("signing_public_sha256", fingerprint) != fingerprint:
        raise ValueError("Device profile and project signing key fingerprints differ")
    profile["signing_public_sha256"] = fingerprint
    private_write(PROFILE, json.dumps(profile, indent=2) + "\n")
    public_hex = public.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.PKCS1).hex()
    csv_path = LOCAL / "identity.csv"
    values = {"ssid": profile["ssid"], "ap_pass": profile["ap_password"], "id": profile["id"],
              "salt": profile["admin_salt"], "admin_hash": profile["admin_hash"], "sign_pub": public_hex}
    with csv_path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["key", "type", "encoding", "value"])
        writer.writerow(["device", "namespace", "", ""])
        for key, value in values.items():
            writer.writerow([key, "data", "string", value])
        writer.writerow(["cert", "file", "string", profile["certificate"]])
        writer.writerow(["tls_key", "file", "string", profile["tls_key"]])
    csv_path.chmod(0o600)
    path = LOCAL / "identity.bin"
    generator = SDK / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    run([runtime(), generator, "generate", csv_path, path, "0x6000"])
    path.chmod(0o600)
    return path


def cmd_provision(args):
    if not PUBLIC_KEY.exists():
        raise ValueError("Run keygen first")
    port = serial_port(args)
    board = inspect_board(port)
    profile = create_identity(board["mac"], reset=args.reset_access)
    image = identity_image(profile)
    tool("esptool", "--chip", "esp32c3", "--port", port, "write-flash", "0x12000", image)
    print("Device identity provisioned. Credentials are in", PROFILE)
    print("Hotspot:", profile["ssid"], " Console: https://192.168.4.1")


def cmd_flash(args):
    cmd_audit(args)
    binary = ROOT / "build/203-entrance.bin"
    image_metadata(binary)
    tool("espsecure", "verify-signature", "--version", "2", "--keyfile", PUBLIC_KEY, binary)
    port = serial_port(args)
    board = inspect_board(port)
    cmd_backup(argparse.Namespace(port=port))
    profile = create_identity(board["mac"])
    identity_binary = identity_image(profile)
    configuration = (ROOT / "sdkconfig").read_text()
    for option in ("CONFIG_SECURE_BOOT", "CONFIG_SECURE_FLASH_ENC_ENABLED", "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK",
                   "CONFIG_SECURE_DISABLE_ROM_DL_MODE", "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE"):
        if re.search(rf"^{option}=y$", configuration, re.MULTILINE):
            raise ValueError(f"Refusing irreversible security configuration: {option}")
    # Initial installation only: a verified full backup precedes erasure.
    # Provision identity and the signed baseline in the same write operation.
    blank = LOCAL / "initial-state.bin"
    initial = bytearray([0xFF]) * 0x17000
    initial[0x9000:0xF000] = identity_binary.read_bytes()
    private_write(blank, initial)
    tool("esptool", "--chip", "esp32c3", "--port", port, "erase-flash")
    tool("esptool", "--chip", "esp32c3", "--port", port, "--baud", "460800", "write-flash",
         "0x0", ROOT / "build/bootloader/bootloader.bin", "0x8000", ROOT / "build/partition_table/partition-table.bin",
         "0x9000", blank, "0x20000", binary)
    print("Initial firmware installed. Join", profile["ssid"], "and run ./entrance status.")
    print("Credentials:", PROFILE)


class Device:
    def __init__(self, host=None):
        self.profile = json.loads(PROFILE.read_text())
        self.host = host or self.profile["host"]
        self.token = None
        self.connection = None

    def request(self, method, path, body=None, *, ticket=None, headers=None, authenticated=True):
        if self.connection is None or self.connection.sock is None:
            context = ssl.create_default_context(cafile=self.profile["certificate"])
            # Identity is additionally pinned, so DHCP station addresses are also usable.
            context.check_hostname = False
            connection = http.client.HTTPSConnection(self.host, timeout=20, context=context)
            connection.connect()
            fingerprint = hashlib.sha256(connection.sock.getpeercert(binary_form=True)).hexdigest()
            if not secrets.compare_digest(fingerprint, self.profile["certificate_sha256"]):
                connection.close()
                raise ValueError("Device certificate pin changed; verify over USB before enrolling a replacement")
            self.connection = connection
        connection = self.connection
        request_headers = dict(headers or {})
        if authenticated and self.token:
            request_headers["Authorization"] = "Bearer " + self.token
        if ticket:
            request_headers["X-Update-Ticket"] = ticket
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            request_headers["Content-Type"] = "application/json"
        try:
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            data = response.read(256 * 1024)
            result = json.loads(data) if data else {}
            if response.status >= 400:
                raise RuntimeError(f"HTTP {response.status}: {result.get('message', result)}")
            return result
        except BaseException:
            connection.close()
            self.connection = None
            raise

    def login(self):
        data = self.request("POST", "/api/v1/session", {"password": self.profile["admin_password"]}, authenticated=False)
        self.token = data["token"]

    def wait_confirmed(self, ticket, expected, timeout=160):
        deadline = time.monotonic() + timeout
        last = "Waiting for reboot"
        while time.monotonic() < deadline:
            try:
                status = self.request("GET", "/api/v1/ota/status", ticket=ticket, authenticated=False)
                if status.get("trial"):
                    self.request("POST", "/api/v1/ota/confirm", ticket=ticket, authenticated=False)
                if status["phase"] == "confirmed" and status["running_version"] == expected:
                    print("Confirmed healthy:", expected)
                    return status
                if status["phase"] in {"failed", "rolled_back"}:
                    raise ValueError("Deployment failed: " + status["detail"])
                current = f"{status['phase']}: {status['detail']}"
                if current != last:
                    print(current, flush=True)
                last = current
            except (OSError, http.client.HTTPException, RuntimeError) as error:
                last = str(error)
            time.sleep(2)
        raise TimeoutError("Firmware did not confirm healthy: " + last)


def cmd_status(args):
    device = Device(args.host)
    device.login()
    print(json.dumps(device.request("GET", "/api/v1/status"), indent=2))


def cmd_upload(args):
    binary = Path(args.image).resolve()
    metadata = image_metadata(binary)
    tool("espsecure", "verify-signature", "--version", "2", "--keyfile", PUBLIC_KEY, binary)
    device = Device(args.host)
    device.login()
    start = device.request("POST", "/api/v1/ota/start", {"size": metadata["size"], "sha256": metadata["sha256"]})
    ticket = start["ticket"]
    try:
        with binary.open("rb") as stream:
            offset = 0
            while chunk := stream.read(start["chunk_size"]):
                device.request("PUT", "/api/v1/ota/chunk", chunk, ticket=ticket,
                               headers={"Content-Type": "application/octet-stream", "X-Upload-Offset": str(offset)})
                offset += len(chunk)
                if offset % (128 * 1024) == 0 or offset == metadata["size"]:
                    print(f"Transferred {offset}/{metadata['size']} bytes", flush=True)
        try:
            device.request("POST", "/api/v1/ota/finish", ticket=ticket)
        except (OSError, http.client.HTTPException):
            # The verified image may already have rebooted before its response
            # arrived. Resolve that uncertainty through its candidate ticket.
            pass
    except BaseException:
        try:
            device.request("POST", "/api/v1/ota/abort")
        except Exception:
            pass
        raise
    if args.no_confirm:
        print("Trial confirmation intentionally withheld; expect rollback after 120 seconds.")
    else:
        device.wait_confirmed(ticket, metadata["version"])


def cmd_network(args):
    device = Device(args.host)
    device.login()
    if args.disable:
        value = {"ssid": "", "password": "", "enabled": False}
    else:
        ssid = args.ssid or input("Uplink SSID: ").strip()
        password = getpass.getpass("Uplink password (not logged): ")
        value = {"ssid": ssid, "password": password, "enabled": True}
    print(device.request("PUT", "/api/v1/network", value))


def cmd_check(args):
    device = Device(args.host)
    device.login()
    body = {"version": validate_version(args.version)} if args.version else None
    device.request("POST", "/api/v1/releases/check", body)
    deadline = time.monotonic() + 90
    time.sleep(2)
    while time.monotonic() < deadline:
        state = device.request("GET", "/api/v1/releases")
        if not state.get("checking"):
            print(json.dumps(state, indent=2))
            if not state.get("verified"):
                raise ValueError("No verified remote release available")
            return
        time.sleep(2)
    raise TimeoutError("Release check timed out")


def cmd_install(args):
    device = Device(args.host)
    device.login()
    release = device.request("GET", "/api/v1/releases")
    if not release.get("verified"):
        raise ValueError("Run ./entrance check first")
    job = device.request("POST", "/api/v1/releases/install")
    device.wait_confirmed(job["ticket"], release["version"], timeout=480)


def cmd_rollback(args):
    device = Device(args.host)
    device.login()
    before = device.request("GET", "/api/v1/status")
    device.request("POST", "/api/v1/ota/rollback")
    time.sleep(5)
    for _ in range(30):
        try:
            device.login()
            after = device.request("GET", "/api/v1/status")
            if after["slot"] != before["slot"] and not after["ota"]["trial"]:
                print("Rollback verified:", after["version"], after["slot"])
                return
        except (OSError, RuntimeError, http.client.HTTPException):
            pass
        time.sleep(2)
    raise TimeoutError("Rollback did not complete")


def cmd_diagnostics(args):
    device = Device(args.host)
    device.login()
    data = device.request("GET", "/api/v1/diagnostics")
    destination = Path(args.output)
    private_write(destination, json.dumps(data, indent=2) + "\n")
    print("Diagnostics saved:", destination)


def cmd_sign(args):
    source = Path(args.image).resolve()
    destination = Path(args.output).resolve()
    if source == destination:
        raise ValueError("Choose a separate signed output file")
    tool("espsecure", "sign-data", "--version", "2", "--keyfile", KEY, "--output", destination, source)
    image_metadata(destination)
    tool("espsecure", "verify-signature", "--version", "2", "--keyfile", PUBLIC_KEY, destination)


def cmd_release(args):
    cmd_audit(args)
    source = Path(args.image).resolve()
    metadata = image_metadata(source)
    if "-" in metadata["version"]:
        raise ValueError("Stable releases must not use prerelease versions")
    if run(["git", "status", "--porcelain"], capture=True).stdout.strip():
        raise ValueError("Release requires a clean source worktree")
    tag = "v" + metadata["version"]
    head = run(["git", "rev-parse", "HEAD"], capture=True).stdout.strip()
    tagged = run(["git", "rev-list", "-n", "1", tag], capture=True).stdout.strip()
    if tagged != head:
        raise ValueError("Version tag must identify the current source commit")
    provenance = json.loads((LOCAL / "build-provenance.json").read_text())
    if (provenance.get("fault") != "none" or provenance["dirty"] or provenance["commit"] != head or
            provenance["inputs_sha256"] != source_fingerprint() or
            provenance["firmware_sha256"] != metadata["sha256"] or provenance["idf"] != IDF_COMMIT):
        raise ValueError("Build provenance does not match this clean tagged source. Rebuild before publication.")
    tool("espsecure", "verify-signature", "--version", "2", "--keyfile", PUBLIC_KEY, source)
    directory = ROOT / "dist" / tag
    directory.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, directory / "firmware.bin")
    raw = manifest_bytes(metadata)
    signature = sign_manifest(raw, KEY)
    verify_manifest(raw, signature, PUBLIC_KEY.read_bytes())
    (directory / "manifest.json").write_bytes(raw)
    (directory / "manifest.sig").write_bytes(signature)
    (directory / "signing-public.pem").write_bytes(PUBLIC_KEY.read_bytes())
    (directory / "source.json").write_text(json.dumps({"repository": "https://github.com/Achxy/203-entrance", "commit": head, "tag": tag, "idf": IDF_COMMIT}, indent=2) + "\n")
    (directory / "SHA256SUMS").write_text("".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n" for p in sorted(directory.iterdir()) if p.name != "SHA256SUMS"))
    print("Verified release bundle:", directory)
    if args.publish:
        notes = LOCAL / "release-notes.md"
        status = "Qualification candidate; remaining hardware gates are documented in the source validation record.\n\n" if args.prerelease else ""
        private_write(notes, f"{status}Signed ESP32-C3 OTA firmware {tag}.\n\nSource: https://github.com/Achxy/203-entrance/tree/{head}\n\nInstall with the local console or `entrance upload`. Bootloader and partition-table updates require USB.\n")
        run(["gh", "release", "create", tag, "--repo", RELEASE_REPO, "--title", tag, "--notes-file", notes,
             *(["--prerelease"] if args.prerelease else []),
             *sorted(directory.iterdir())])


def cmd_test(args):
    run([runtime(), "-m", "unittest", "discover", "-s", "tests", "-v"])


def cmd_audit(args):
    # Inspect the configuration emitted for the compiled binary, not a possibly
    # edited sdkconfig belonging to a future build.
    configuration = json.loads((ROOT / "build/config/sdkconfig.json").read_text())
    for option in ("SECURE_BOOT", "SECURE_FLASH_ENC_ENABLED", "BOOTLOADER_APP_ANTI_ROLLBACK",
                   "SECURE_DISABLE_ROM_DL_MODE", "SECURE_ENABLE_SECURE_ROM_DL_MODE"):
        if configuration.get(option) is True:
            raise ValueError(f"Recovery policy violation: {option}")
    for option in ("SECURE_SIGNED_APPS_NO_SECURE_BOOT", "SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT",
                   "SECURE_SIGNED_APPS_RSA_SCHEME", "BOOTLOADER_APP_ROLLBACK_ENABLE",
                   "ESP_COREDUMP_ENABLE_TO_FLASH", "ESPTOOLPY_FLASHSIZE_4MB",
                   "ESP_TASK_WDT_EN", "ESP_TASK_WDT_INIT", "ESP_TASK_WDT_PANIC"):
        if configuration.get(option) is not True:
            raise ValueError(f"Required firmware policy missing: {option}")
    if configuration.get("CJSON_NESTING_LIMIT") != 8:
        raise ValueError("JSON nesting limit must be bounded")
    if configuration.get("ESP_TASK_WDT_TIMEOUT_S") != 15:
        raise ValueError("Health task watchdog must reset an unresponsive trial after 15 seconds")
    rows = []
    for row in csv.reader((ROOT / "partitions.csv").read_text().splitlines()):
        if row and not row[0].strip().startswith("#"):
            rows.append([item.strip() for item in row])
    expected = {"nvs": (0x9000, 0x7000), "otadata": (0x10000, 0x2000),
                "identity": (0x12000, 0x6000), "state": (0x18000, 0x8000),
                "ota_0": (0x20000, SLOT_BYTES), "ota_1": (0x200000, SLOT_BYTES),
                "coredump": (0x3E0000, 0x20000)}
    actual = {row[0]: (int(row[3], 0), int(row[4], 0)) for row in rows}
    if actual != expected:
        raise ValueError("Partition layout differs from signed compatibility layout 1")
    if not any(row[0] == "identity" and len(row) > 5 and row[5] == "readonly" for row in rows):
        raise ValueError("Identity partition must be read-only in application firmware")
    binary = ROOT / "build/203-entrance.bin"
    metadata = image_metadata(binary)
    tool("espsecure", "verify-signature", "--version", "2", "--keyfile", PUBLIC_KEY, binary)
    print("Recovery, signature, partition and bounded-parser policies passed.")
    print("Signed size:", metadata["size"], "of", SLOT_BYTES)


def cmd_monitor(args):
    idf("-p", serial_port(args), "monitor")


def cmd_soak(args):
    if args.serial:
        import serial
        rows = []
        deadline = time.monotonic() + args.minutes * 60
        last_sample = time.monotonic()
        with serial.Serial(serial_port(args), 115200, timeout=1) as port:
            while time.monotonic() < deadline:
                line = port.readline().decode(errors="replace")
                if "HEALTH {" in line:
                    row = json.loads(line.split("HEALTH ", 1)[1].split("\x1b", 1)[0])
                    if rows and (row["uptime_ms"] <= rows[-1]["uptime_ms"] or row["version"] != rows[-1]["version"]):
                        raise ValueError("Unexpected reset or firmware change during soak")
                    if not row["services"] or row["trial"]:
                        raise ValueError("Unhealthy or unconfirmed firmware during soak")
                    rows.append(row)
                    last_sample = time.monotonic()
                    private_write(Path(args.output), json.dumps(rows, indent=2) + "\n")
                    print(f"uptime={row['uptime_ms']/1000:.0f}s heap={row['free_heap']}", flush=True)
                if time.monotonic() - last_sample > 45:
                    raise TimeoutError("No health telemetry for 45 seconds")
        if len(rows) < max(1, args.minutes * 4 - 2):
            raise ValueError("Insufficient telemetry for the requested soak")
        print("Serial soak complete:", len(rows), "samples")
        return
    device = Device(args.host)
    device.login()
    deadline = time.monotonic() + args.minutes * 60
    rows = []
    previous = None
    while time.monotonic() < deadline:
        current = device.request("GET", "/api/v1/status")
        if previous and current["uptime_ms"] < previous["uptime_ms"]:
            raise ValueError("Unexpected reset during soak")
        rows.append({k: current[k] for k in ("version", "slot", "uptime_ms", "free_heap", "minimum_heap", "largest_block", "reset_reason")})
        previous = current
        private_write(Path(args.output), json.dumps(rows, indent=2) + "\n")
        print(f"uptime={current['uptime_ms']/1000:.0f}s heap={current['free_heap']} minimum={current['minimum_heap']}", flush=True)
        time.sleep(15)
        if len(rows) % 200 == 0:
            device.login()
    print("Soak samples saved:", args.output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    handlers = {"setup": cmd_setup, "doctor": cmd_doctor, "keygen": cmd_keygen, "build": cmd_build,
                "test": cmd_test, "audit": cmd_audit, "backup": cmd_backup, "provision": cmd_provision, "flash": cmd_flash,
                "status": cmd_status, "upload": cmd_upload, "network": cmd_network, "check": cmd_check,
                "install": cmd_install, "rollback": cmd_rollback, "diagnostics": cmd_diagnostics,
                "sign": cmd_sign, "release": cmd_release, "monitor": cmd_monitor, "soak": cmd_soak}
    for name, handler in handlers.items():
        p = sub.add_parser(name)
        p.set_defaults(handler=handler)
        if name in {"backup", "provision", "flash", "monitor", "soak"}:
            p.add_argument("--port")
        if name in {"status", "upload", "network", "check", "install", "rollback", "diagnostics", "soak"}:
            p.add_argument("--host")
        if name == "build":
            p.add_argument("--version", default="0.1.0")
            p.add_argument("--fault", choices=["none", "startup", "watchdog", "reset", "activation"], default="none",
                           help="Qualification-only fault during trial; release tooling refuses these builds")
        if name == "provision": p.add_argument("--reset-access", action="store_true", help="Generate replacement device access credentials and certificate")
        if name in {"upload", "sign", "release"}: p.add_argument("image")
        if name == "upload": p.add_argument("--no-confirm", action="store_true", help="Hardware fault test: intentionally require rollback")
        if name == "network":
            p.add_argument("--ssid")
            p.add_argument("--disable", action="store_true")
        if name == "diagnostics": p.add_argument("--output", default=str(LOCAL / "diagnostics.json"))
        if name == "sign": p.add_argument("--output", required=True)
        if name == "check": p.add_argument("--version", help="Explicit versioned release check; default checks latest stable")
        if name == "release":
            p.add_argument("--publish", action="store_true")
            p.add_argument("--prerelease", action="store_true", help="Publish a qualification candidate excluded from automatic stable checks")
        if name == "soak":
            p.add_argument("--serial", action="store_true", help="Collect USB health telemetry while retaining the host Internet connection")
            p.add_argument("--minutes", type=int, default=60)
            p.add_argument("--output", default=str(LOCAL / "soak.json"))
    args = parser.parse_args()
    try:
        args.handler(args)
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
