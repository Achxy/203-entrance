#!/usr/bin/env python3
"""Build an isolated lighting candidate; never flash, publish or provision."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from entrance import idf_env, runtime  # noqa: E402
from firmware import IDF_COMMIT, SLOT_BYTES, VERSION, image_metadata, validate_version  # noqa: E402

SDK = ROOT / ".tools/esp-idf"
OUT = ROOT / ".local/lights-candidate"
BUILD = OUT / "build"
CONFIG = OUT / "sdkconfig"
KEY = OUT / "disposable-signing.pem"
PUBLIC_KEY = OUT / "disposable-signing-public.pem"
IMAGE_LIMIT = 0x1C0000
PLACEMENT = {
    "ESP_WIFI_IRAM_OPT": "n", "ESP_WIFI_RX_IRAM_OPT": "n",
    "ESP32_WIFI_IRAM_OPT": "n", "ESP32_WIFI_RX_IRAM_OPT": "n",
    "WIFI_RMT_IRAM_OPT": "n", "WIFI_RMT_RX_IRAM_OPT": "n",
    "BT_NIMBLE_LOW_SPEED_MODE": "y", "BT_CTRL_RUN_IN_FLASH_ONLY": "y",
    "SPI_FLASH_AUTO_SUSPEND": "y", "SPI_FLASH_ROM_IMPL": "n",
}
RESOURCE_LIMITS = {"LWIP_MAX_SOCKETS": "20"}
PROTECTED = [ROOT / name for name in (
    "sdkconfig", "sdkconfig.old", "build/203-entrance.bin",
    "build/203-entrance.elf", "build/203-entrance.map", "build/CMakeCache.txt",
)]


@dataclass(frozen=True)
class BuildSelection:
    version: str
    output: Path
    signing_key: Path
    public_key: Path
    hardware: bool = False
    lighting_enabled: bool = True


def select_build(root: Path, version: str | None, *, default_off: bool = False,
                 hardware: bool = False) -> BuildSelection:
    if default_off and hardware:
        raise ValueError("Hardware qualification and default-off modes cannot be combined")
    if hardware:
        if not version:
            raise ValueError("Hardware qualification requires an explicit --version prerelease")
        version = validate_version(version)
        if not VERSION.fullmatch(version)[4]:
            raise ValueError("Hardware qualification requires a prerelease version, not a stable release")
        return BuildSelection(version, root / ".local/lights-hardware" / version,
                              root / ".local/keys/firmware.pem", root / "keys/release-public.pem", True)
    version = validate_version(version or ("0.3.0-lights.off" if default_off else "0.3.0-lights.dev"))
    output = root / (".local/lights-default-off" if default_off else ".local/lights-candidate")
    return BuildSelection(version, output, output / "disposable-signing.pem",
                          output / "disposable-signing-public.pem", lighting_enabled=not default_off)


def version_is_newer(candidate: str, baseline: str) -> bool:
    """SemVer precedence; build metadata cannot bypass the downgrade guard."""
    first, second = (VERSION.fullmatch(validate_version(value)) for value in (candidate, baseline))
    core_a, core_b = (tuple(int(match[i]) for i in (1, 2, 3)) for match in (first, second))
    if core_a != core_b:
        return core_a > core_b
    pre_a, pre_b = first[4], second[4]
    if pre_a is None or pre_b is None:
        return pre_a is None and pre_b is not None
    for a, b in zip(pre_a.split("."), pre_b.split(".")):
        if a == b:
            continue
        if a.isdigit() and b.isdigit():
            return int(a) > int(b)
        if a.isdigit() != b.isdigit():
            return not a.isdigit()
        return a > b
    return len(pre_a.split(".")) > len(pre_b.split("."))


def clean_revision(root: Path) -> dict:
    def git(*args: str) -> str:
        return subprocess.check_output(["git", *args], cwd=root, text=True).strip()

    if git("status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none"):
        raise RuntimeError("Hardware qualification requires a clean committed source worktree")
    git("ls-files", "--error-unmatch", "keys/release-public.pem", "lights/tools/build_candidate.py")
    return {"commit": git("rev-parse", "HEAD"), "tree": git("rev-parse", "HEAD^{tree}"),
            "tags": git("tag", "--points-at", "HEAD").splitlines(), "clean": True}


def checked_production_key(selection: BuildSelection) -> str:
    """Read and compare existing keys only; never create, copy or chmod them."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    if not selection.hardware:
        raise ValueError("Production-key validation requires hardware-qualification mode")
    key_path, public_path = selection.signing_key, selection.public_key
    for path in (key_path, public_path):
        if path.resolve(strict=True) != path or not stat.S_ISREG(path.lstat().st_mode):
            raise RuntimeError("Signing keys must be existing regular files without symlinks")
    key_stat = key_path.stat()
    if key_stat.st_uid != os.getuid() or stat.S_IMODE(key_stat.st_mode) not in (0o400, 0o600):
        raise RuntimeError("Production signing key must be owned by this user with mode 0400 or 0600")
    for parent in (key_path.parent, key_path.parent.parent):
        info = parent.stat()
        if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) & 0o077:
            raise RuntimeError("Production signing-key directories must be private to their owner")
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
    public = serialization.load_pem_public_key(public_path.read_bytes())
    if (not isinstance(key, rsa.RSAPrivateKey) or key.key_size != 3072 or
        not isinstance(public, rsa.RSAPublicKey) or key.public_key().public_numbers() != public.public_numbers()):
        raise RuntimeError("Existing RSA-3072 signing key does not match keys/release-public.pem")
    canonical = key.public_key().public_bytes(serialization.Encoding.DER,
                                            serialization.PublicFormat.SubjectPublicKeyInfo)
    return hashlib.sha256(canonical).hexdigest()


def signer_snapshot(selection: BuildSelection) -> tuple:
    # Retain key-file identity privately in memory, never in the public provenance.
    return tuple((path.stat().st_dev, path.stat().st_ino, path.stat().st_mtime_ns,
                  path.stat().st_mode, digest(path))
                 for path in (selection.signing_key, selection.public_key))


def dependency_fingerprints(paths: list[Path]) -> dict[str, str]:
    result = {}
    for directory in paths:
        tree = hashlib.sha256()
        for path in sorted(directory.rglob("*")):
            if path.is_symlink():
                raise RuntimeError("Resolved managed dependencies must not contain symlinks")
            if path.is_file():
                tree.update(path.relative_to(directory).as_posix().encode() + b"\0")
                tree.update(path.read_bytes() + b"\0")
        result[directory.name] = tree.hexdigest()
    return result


def prepare_private_output(root: Path, output: Path) -> None:
    relative = output.relative_to(root)
    if relative.parts[0] != ".local" or ".." in relative.parts:
        raise RuntimeError("Hardware output must stay in the private local directory")
    current = root
    for part in relative.parts:
        current /= part
        if current.is_symlink():
            raise RuntimeError("Refusing symlink hardware output directory")
        current.mkdir(mode=0o700, exist_ok=True)
        info = current.stat()
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or
            stat.S_IMODE(info.st_mode) & 0o077):
            raise RuntimeError("Hardware output directories must be private to their owner")


def check_build_directory(output: Path) -> None:
    build = output / "build"
    if (build.is_symlink() or build.resolve() != build or
        (build.exists() and not build.is_dir())):
        raise RuntimeError("Candidate build directory must be a real isolated directory")


def check_hardware_output(output: Path, version: str, revision: dict, provenance: dict) -> None:
    previous = output / "summary.json"
    if previous.is_symlink():
        raise RuntimeError("Refusing symlink hardware provenance")
    if previous.exists():
        summary = json.loads(previous.read_text())
        if (summary.get("mode") != "hardware-qualification" or
            summary.get("metadata", {}).get("version") != version or
            summary.get("revision", {}).get("commit") != revision["commit"] or
            any(summary.get(field) != expected for field, expected in provenance.items())):
            raise RuntimeError("This hardware version already names different inputs; choose a newer prerelease")


def digest(path: Path) -> str | None:
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def source_fingerprint() -> str:
    # Include tracked and new firmware inputs, but not unrelated native/web-site
    # edits that cannot affect this build. Embedded web/player assets are inputs.
    paths = ("CMakeLists.txt", "partitions.csv", "sdkconfig.defaults", "dependencies.lock",
             "main", "components", "web", "player", "assets", "keys", "lights/firmware",
             "lights/tools/build_candidate.py")
    listed = subprocess.check_output(["git", "ls-files", "--cached", "--others",
                                      "--exclude-standard", "-z", "--", *paths], cwd=ROOT)
    result = hashlib.sha256()
    for name in sorted(set(listed.decode().split("\0")) - {""}):
        path = ROOT / name
        if (path.suffix == ".md" or path.name in {".gitignore", ".gitkeep"} or
            any(part in {".build", ".idf-build", ".cache", "build", "__pycache__", ".swiftpm", "tests"}
                for part in Path(name).parts)):
            continue
        result.update(name.encode() + b"\0")
        result.update(path.read_bytes() if path.is_file() else b"<deleted>")
        result.update(b"\0")
    return result.hexdigest()


def replace_setting(config: str, key: str, value: str) -> str:
    pattern = re.compile(r"(?:# )?CONFIG_" + re.escape(key) + r"(?:=.*| is not set)$")
    lines = [line for line in config.splitlines() if not pattern.fullmatch(line)]
    lines.append(f"# CONFIG_{key} is not set" if value == "n" else f"CONFIG_{key}={value}")
    return "\n".join(lines) + "\n"


def candidate_config(production: str, ble_defaults: str, signing_key: Path | None = None) -> str:
    result = production
    for line in ble_defaults.splitlines():
        match = re.fullmatch(r"(?:# )?CONFIG_([A-Z0-9_]+)(?:=(.*)| is not set)", line)
        if match and (match[1].startswith(("BT_", "ESP_PROTOCOMM_")) or
                      match[1] == "ESP_COEX_SW_COEXIST_ENABLE"):
            result = replace_setting(result, match[1], match[2] if match[2] is not None else "n")
    for key, value in (PLACEMENT | RESOURCE_LIMITS).items():
        result = replace_setting(result, key, value)
    return replace_setting(result, "SECURE_BOOT_SIGNING_KEY", json.dumps(str(signing_key or KEY)))


def build_config(production: str, ble_defaults: str, selection: BuildSelection) -> str:
    if selection.lighting_enabled:
        return candidate_config(production, ble_defaults, selection.signing_key)
    return replace_setting(production, "SECURE_BOOT_SIGNING_KEY", json.dumps(str(selection.signing_key)))


def private_write(path: Path, data: bytes) -> None:
    if path.is_symlink():
        raise RuntimeError(f"Refusing symlink output: {path}")
    temporary = path.with_name(path.name + ".tmp")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


@contextmanager
def build_lock(hardware: bool = False):
    if hardware:
        prepare_private_output(ROOT, OUT)
    else:
        OUT.mkdir(mode=0o700, parents=True, exist_ok=True)
    if OUT.is_symlink() or OUT.resolve() != OUT:
        raise RuntimeError("Candidate output directory must not be a symlink")
    check_build_directory(OUT)
    path = OUT / ".build.lock"
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("Another candidate build holds .build.lock; wait for it to finish") from error
        yield
    finally:
        os.close(fd)


def prepare_test_key() -> None:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    if KEY.is_symlink():
        raise RuntimeError("Refusing a symlink signing key")
    if KEY.exists():
        key = serialization.load_pem_private_key(KEY.read_bytes(), password=None)
        if not isinstance(key, rsa.RSAPrivateKey) or key.key_size != 3072:
            raise RuntimeError("Existing disposable key is not RSA-3072; refusing to replace it")
    else:
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        private_write(KEY, key.private_bytes(serialization.Encoding.PEM,
                      serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    KEY.chmod(0o600)
    private_write(PUBLIC_KEY, key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))


def execute(arguments: list, log: Path, environment: dict) -> None:
    if log.is_symlink():
        raise RuntimeError(f"Refusing symlink log: {log}")
    descriptor = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    os.fchmod(descriptor, 0o600)
    with os.fdopen(descriptor, "w") as stream:
        result = subprocess.run([str(value) for value in arguments], cwd=ROOT,
                                env=environment, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        print("\n".join(log.read_text(errors="replace").splitlines()[-60:]), file=sys.stderr)
        raise RuntimeError(f"Command failed ({result.returncode}); full output: {log}")


def validate_resolved_configuration(lighting_enabled: bool = True) -> None:
    values = json.loads((BUILD / "config/sdkconfig.json").read_text())
    required_true = (
        "BT_ENABLED", "BT_NIMBLE_ENABLED", "BT_NIMBLE_ROLE_PERIPHERAL",
        "ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2", "ESP_COEX_SW_COEXIST_ENABLE",
        "BT_NIMBLE_LOW_SPEED_MODE", "BT_CTRL_RUN_IN_FLASH_ONLY", "SPI_FLASH_AUTO_SUSPEND",
        "SECURE_SIGNED_APPS_NO_SECURE_BOOT", "SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT",
        "SECURE_SIGNED_APPS_RSA_SCHEME", "SECURE_BOOT_BUILD_SIGNED_BINARIES",
        "BOOTLOADER_APP_ROLLBACK_ENABLE",
    )
    required_false = (
        "ESP_WIFI_IRAM_OPT", "ESP_WIFI_RX_IRAM_OPT", "SPI_FLASH_ROM_IMPL",
        "SECURE_BOOT", "SECURE_FLASH_ENC_ENABLED", "BOOTLOADER_APP_ANTI_ROLLBACK",
        "ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_0", "ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1",
        "BT_BLUEDROID_ENABLED", "BT_NIMBLE_ROLE_CENTRAL", "BT_NIMBLE_ROLE_OBSERVER",
    )
    if not lighting_enabled:
        required_true = tuple(name for name in required_true if name.startswith(("SECURE_", "BOOTLOADER_")))
        required_false = ("SECURE_BOOT", "SECURE_FLASH_ENC_ENABLED", "BOOTLOADER_APP_ANTI_ROLLBACK", "BT_ENABLED")
    for name in required_true:
        if values.get(name) is not True:
            raise RuntimeError(f"Required candidate setting is not enabled: {name}")
    for name in required_false:
        if values.get(name, False) is not False:
            raise RuntimeError(f"Unsafe/unexpected candidate setting enabled: {name}")
    if values.get("SECURE_BOOT_SIGNING_KEY") != str(KEY):
        raise RuntimeError("Resolved signing key is not the isolated disposable key")
    expected_values = {"IDF_TARGET": "esp32c3"}
    if lighting_enabled:
        expected_values.update(BT_NIMBLE_MAX_CONNECTIONS=2, BT_NIMBLE_ATT_PREFERRED_MTU=128,
                               LWIP_MAX_SOCKETS=20)
    for name, expected in expected_values.items():
        if values.get(name) != expected:
            raise RuntimeError(f"Unexpected resolved {name}")
    if not lighting_enabled:
        return
    commands = json.loads((BUILD / "compile_commands.json").read_text())
    for name in ("security2.c", "esp_srp.c", "esp_srp_mpi.c"):
        relevant = [item for item in commands if Path(item["file"]).name == name]
        if not relevant or any("-DLOG_LOCAL_LEVEL=ESP_LOG_INFO" not in item["command"].split()
                               for item in relevant):
            raise RuntimeError(f"Sensitive SDK debug logging is not compile-capped for {name}")


def target_type_sizes() -> dict[str, int]:
    """Read the target ABI without executing the candidate or host-size guesses."""
    from elftools.elf.elffile import ELFFile

    wanted = {"runtime_t", "lgs_service_t", "wiz_driver_t", "wiz_esp_idf_t",
              "ls_manager_t", "lrq_t", "lb_event_t", "light_reply_t"}
    sizes: dict[str, int] = {}
    with (BUILD / "203-entrance.elf").open("rb") as stream:
        for unit in ELFFile(stream).get_dwarf_info().iter_CUs():
            for entry in unit.iter_DIEs():
                attribute = entry.attributes.get("DW_AT_name")
                name = attribute.value.decode(errors="replace") if attribute else ""
                if name not in wanted:
                    continue
                target = entry.get_DIE_from_attribute("DW_AT_type") if entry.tag == "DW_TAG_typedef" else entry
                size = target.attributes.get("DW_AT_byte_size")
                if size:
                    if name in sizes and sizes[name] != size.value:
                        raise RuntimeError(f"Inconsistent target ABI for {name}; rebuild all dependent objects")
                    sizes[name] = size.value
    if not {"runtime_t", "lgs_service_t", "wiz_driver_t", "wiz_esp_idf_t"} <= sizes.keys():
        raise RuntimeError("Expected lighting runtime types are absent from the target ELF")
    return sizes


def main() -> None:
    global OUT, BUILD, CONFIG, KEY, PUBLIC_KEY
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", help="candidate SemVer; hardware qualification requires an explicit newer prerelease")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--default-off", action="store_true", help="separate disposable-key regression build with ENTRANCE_LIGHTS=OFF")
    modes.add_argument("--hardware-candidate", action="store_true", help="isolated local qualification with the existing production key; requires clean committed sources")
    parser.add_argument("--dry-run", action="store_true", help="show paths/flags without writing or building")
    args = parser.parse_args()
    selection = select_build(ROOT, args.version, default_off=args.default_off, hardware=args.hardware_candidate)
    version, OUT, KEY, PUBLIC_KEY = selection.version, selection.output, selection.signing_key, selection.public_key
    BUILD, CONFIG = OUT / "build", OUT / "sdkconfig"
    baseline = image_metadata(ROOT / "build/203-entrance.bin") if selection.hardware else None
    if baseline and not version_is_newer(version, baseline["version"]):
        raise ValueError(f"Hardware prerelease must be newer than retained baseline {baseline['version']}")
    managed = [ROOT / "managed_components" / name for name in
               ("espressif__cjson", "espressif__esp_websocket_client")]
    command = [runtime(), SDK / "tools/idf.py", "-C", ROOT, "-B", BUILD,
               "-D", f"SDKCONFIG={CONFIG}", "-D", "IDF_TARGET=esp32c3",
               "-D", f"PROJECT_VER={version}", "-D", "ENTRANCE_TEST_FAULT=0",
               "-D", "ENTRANCE_LIGHTS=" + ("ON" if selection.lighting_enabled else "OFF"),
               "-D", "EXTRA_COMPONENT_DIRS=" + ";".join(map(str, managed)),
               "build"]
    if args.dry_run:
        print(json.dumps({"command": list(map(str, command)), "output": str(OUT),
                          "signing_key": str(KEY), "component_manager": False,
                          "maximum_signed_bytes": IMAGE_LIMIT, "lighting_enabled": selection.lighting_enabled,
                          "hardware_qualification_only": selection.hardware,
                          "checks_not_run": ["source cleanliness", "key permissions and identity", "signature", "build"],
                          "flashes": False}, indent=2))
        return
    if not (ROOT / "sdkconfig").is_file() or any(not path.is_dir() for path in managed):
        raise RuntimeError("Existing production sdkconfig and resolved managed components are required")
    pinned_python = ROOT / ".tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python"
    if not pinned_python.is_file() or Path(sys.prefix) != pinned_python.parent.parent:
        raise RuntimeError(f"Run with the pinned environment: {pinned_python} {Path(__file__)}")
    actual = subprocess.check_output(["git", "-C", str(SDK), "rev-parse", "HEAD"], text=True).strip()
    if actual != IDF_COMMIT:
        raise RuntimeError(f"Pinned SDK mismatch: {actual}")
    if subprocess.check_output(["git", "-C", str(SDK), "diff", "HEAD", "--name-only"], text=True).strip():
        raise RuntimeError("Pinned SDK contains tracked changes; use a separate reviewed component instead")
    revision = clean_revision(ROOT) if selection.hardware else None
    signing_fingerprint = checked_production_key(selection) if selection.hardware else None
    signer_before = signer_snapshot(selection) if selection.hardware else None
    before = {str(path.relative_to(ROOT)): digest(path) for path in PROTECTED}
    with build_lock(selection.hardware):
        try:
            inputs = source_fingerprint()
            dependencies = dependency_fingerprints(managed)
            if not selection.hardware:
                prepare_test_key()
            production = (ROOT / "sdkconfig").read_text()
            profile = build_config(production, (ROOT / "lights/firmware/probe/sdkconfig.defaults").read_text(), selection)
            if selection.hardware:
                check_hardware_output(OUT, version, revision, {
                    "source_sha256": inputs, "managed_dependencies_sha256": dependencies,
                    "configuration_input_sha256": hashlib.sha256(profile.encode()).hexdigest(),
                    "signing_public_der_sha256": signing_fingerprint,
                })
            private_write(CONFIG, profile.encode())
            environment = idf_env()
            environment["IDF_COMPONENT_MANAGER"] = "0"
            if selection.hardware:
                execute([runtime(), "-m", "espsecure", "verify-signature", "--version", "2",
                         "--keyfile", PUBLIC_KEY, ROOT / "build/203-entrance.bin"],
                        OUT / "baseline-signature-check.log", environment)
            purpose = "hardware qualification (production key)" if selection.hardware else "test-key candidate"
            print(f"Building isolated {purpose}; log: {OUT / 'build.log'}", flush=True)
            execute(command, OUT / "build.log", environment)
            validate_resolved_configuration(selection.lighting_enabled)
            binary = BUILD / "203-entrance.bin"
            metadata = image_metadata(binary)
            if metadata["version"] != version or metadata["size"] > IMAGE_LIMIT:
                raise RuntimeError("Candidate version or signed-image reserve gate failed")
            execute([runtime(), "-m", "espsecure", "verify-signature", "--version", "2",
                     "--keyfile", PUBLIC_KEY, binary], OUT / "signature-check.log", environment)
            for mode, flags in (("size", []), ("archives", ["--archives"])):
                execute([runtime(), "-m", "esp_idf_size", *flags, "--format", "json2",
                         BUILD / "203-entrance.map"], OUT / f"{mode}.json", environment)
            types = target_type_sizes() if selection.lighting_enabled else {}
            private_write(OUT / "type-sizes.json", (json.dumps({"elf_sha256": digest(BUILD / "203-entrance.elf"),
                          "types": types, "flashed": False}, indent=2) + "\n").encode())
            if inputs != source_fingerprint():
                raise RuntimeError("Repository sources changed during this build; rerun when edits finish")
            if dependencies != dependency_fingerprints(managed):
                raise RuntimeError("Resolved managed dependencies changed during the build")
            if selection.hardware and (clean_revision(ROOT) != revision or
                                       signer_snapshot(selection) != signer_before):
                raise RuntimeError("Hardware source revision or signing-key files changed during the build")
            if before != {str(path.relative_to(ROOT)): digest(path) for path in PROTECTED}:
                raise RuntimeError("Production configuration/build changed during candidate build; inspect before proceeding")
            summary = {"created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                       "idf": actual, "source_sha256": inputs, "metadata": metadata,
                       "managed_dependencies_sha256": dependencies, "revision": revision,
                       "mode": "hardware-qualification" if selection.hardware else "disposable-key",
                       "signing_public_der_sha256": signing_fingerprint,
                       "retained_baseline": baseline,
                       "slot_headroom_bytes": SLOT_BYTES - metadata["size"],
                       "reserve_gate_bytes": IMAGE_LIMIT, "test_key_only": not selection.hardware,
                       "hardware_qualification_only": selection.hardware, "publication_permitted": False,
                       "lighting_enabled": selection.lighting_enabled,
                       "flashed": False, "runtime_qualified": False,
                       "configuration_input_sha256": hashlib.sha256(profile.encode()).hexdigest(),
                       "configuration_sha256": digest(CONFIG),
                       "target_type_sizes": types,
                       "size": json.loads((OUT / "size.json").read_text()),
                       "production_artifacts_before": before}
            private_write(OUT / "summary.json", (json.dumps(summary, indent=2) + "\n").encode())
            print(json.dumps({"signed_bytes": metadata["size"], "headroom_bytes": summary["slot_headroom_bytes"],
                              "sha256": metadata["sha256"], "summary": str(OUT / "summary.json")}, indent=2))
            print("Compile/signature checks passed. " +
                  ("Production key; local qualification only, not installed or hardware-qualified."
                   if selection.hardware else "Test key only; not installed or hardware-qualified."))
        finally:
            after = {str(path.relative_to(ROOT)): digest(path) for path in PROTECTED}
            if before != after:
                raise RuntimeError("Production configuration/build changed during candidate build; inspect before proceeding")
            if selection.hardware and signer_snapshot(selection) != signer_before:
                raise RuntimeError("Production signing-key files changed during candidate build; inspect before proceeding")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Candidate build stopped: {error}", file=sys.stderr)
        raise SystemExit(1)
