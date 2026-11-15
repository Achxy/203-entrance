#!/usr/bin/env python3
"""Snapshot production sources into ignored storage and measure candidate links.

Never flashes. All images use a disposable signing key and are non-deployable to
the production board. No production config, source, build or credential changes.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

from measure import PROJECT, ROOT, OUT, SDK, IDF_COMMIT, runtime, idf_env, execute

def replace_config(text, key, value):
    lines = [line for line in text.splitlines()
             if not re.match(r"(?:# )?CONFIG_"+re.escape(key)+r"(?:=| is not set$)", line)]
    lines.append(f"# CONFIG_{key} is not set" if value == "n" else f"CONFIG_{key}={value}")
    return "\n".join(lines)+"\n"

def main():
    args = argparse.ArgumentParser()
    args.add_argument("--with-controller-flash", action="store_true")
    args.add_argument("--with-transport", action="store_true")
    args.add_argument("--with-gateway", action="store_true")
    args.add_argument("--only", choices=("combined-default", "wifi-flash", "wifi-bt-flash", "wifi-bt-suspend"))
    selected = args.parse_args()
    if selected.only in ("wifi-bt-flash", "wifi-bt-suspend"):
        selected.with_controller_flash = True
    actual = subprocess.check_output(["git", "-C", str(SDK), "rev-parse", "HEAD"], text=True).strip()
    if actual != IDF_COMMIT: raise SystemExit("Pinned SDK mismatch")
    location = OUT / "combined"
    source = location / "source"
    build = location / "build"
    source.mkdir(parents=True, exist_ok=True)
    copied = {}
    for directory in ("main", "components/entrance"):
        shutil.copytree(ROOT / directory, source / directory, dirs_exist_ok=True)
        for path in (ROOT / directory).rglob("*"):
            if path.is_file(): copied[str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    shutil.copytree(ROOT / "lights/firmware/session", source / "components/session", dirs_exist_ok=True)
    if selected.with_transport:
        shutil.copytree(ROOT / "lights/firmware/ble", source / "components/ble", dirs_exist_ok=True)
    if selected.with_gateway:
        for name in ("gateway", "core", "wiz"):
            shutil.copytree(ROOT / "lights/firmware" / name, source / "components" / name, dirs_exist_ok=True)
    for directory in ("lights/firmware/session", "lights/firmware/core", "lights/firmware/ble",
                      "lights/firmware/gateway", "lights/firmware/wiz"):
        for path in (ROOT / directory).glob("*"):
            if path.is_file(): copied[str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    for name in ("web", "assets", "player", "keys"):
        target = source / name
        if not target.exists(): target.symlink_to(ROOT / name, target_is_directory=True)
    shutil.copy2(ROOT / "partitions.csv", source / "partitions.csv")
    shutil.copy2(PROJECT / "main/probe.c", source / "main/resource_probe.c")
    for name in ("lighting_authority.c", "lighting_authority.h"):
        shutil.copy2(ROOT / "lights/firmware/core" / name, source / "main" / name)
    main_c = (source / "main/main.c").read_text()
    main_c = 'void lights_resource_probe(void);\n'+main_c
    # Link-only fixture: never production; invoke probe after normal startup.
    at = main_c.rfind("}")
    main_c = main_c[:at]+"    lights_resource_probe();\n"+main_c[at:]
    (source / "main/main.c").write_text(main_c)
    main_cmake = '''idf_component_register(
    SRCS "main.c" "resource_probe.c" "lighting_authority.c"
    INCLUDE_DIRS "." REQUIRES entrance bt protocomm session)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROBE_STAGE=3
    PROBE_ENTRY=lights_resource_probe PROBE_INCLUDE_AUTHORITY=1)
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,lg_submit,-u,lg_complete_target,-u,lg_prepare_send,-u,lg_observe"
    "-Wl,-u,lg_policy_encode,-u,lg_policy_decode,-u,lg_stage_freeze"
    "-Wl,-u,lg_stage_revoke,-u,lg_stage_rotate_guest_epoch,-u,lg_stage_admit_guest"
    "-Wl,-u,lg_set_enabled_after_commit,-u,lg_command_status,-u,lg_snapshot")
'''
    if selected.with_transport:
        main_cmake=main_cmake.replace("REQUIRES entrance bt protocomm session)",
                                       "REQUIRES entrance bt protocomm session ble)")
        main_cmake += "target_compile_definitions(${COMPONENT_LIB} PRIVATE PROBE_USE_TRANSPORT=1)\n"
    if selected.with_gateway:
        main_cmake = main_cmake.replace('"resource_probe.c" "lighting_authority.c"', '"resource_probe.c"')
        main_cmake = main_cmake.replace('session ble)', 'session ble gateway wiz)')
        main_cmake = main_cmake.replace('session)', 'session gateway wiz)')
        main_cmake = main_cmake.replace('PROBE_INCLUDE_AUTHORITY=1', 'PROBE_INCLUDE_GATEWAY=1')
        main_cmake += '''target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,lgs_service_init,-u,lgs_provision_trusted_admin,-u,lgs_service_tick"
    "-Wl,-u,lgs_service_request,-u,lgs_service_close_route,-u,lgs_adopt_candidate"
    "-Wl,-u,wiz_esp_idf_init,-u,wiz_esp_idf_peers,-u,lgs_nvs_open,-u,lgs_nvs_close")
'''
    (source / "main/CMakeLists.txt").write_text(main_cmake)
    cmake = (ROOT / "CMakeLists.txt").read_text()
    extra = '\nset(EXTRA_COMPONENT_DIRS "'+str(ROOT / 'managed_components/espressif__cjson')+'" "'+str(ROOT / 'managed_components/espressif__esp_websocket_client')+'")\n'
    cmake = cmake.replace('include($ENV{IDF_PATH}', extra+'include($ENV{IDF_PATH}')
    cmake += '''\nforeach(name protocomm esp_security)
    idf_component_get_property(component_lib ${name} COMPONENT_LIB)
    target_compile_definitions(${component_lib} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_INFO)
endforeach()
'''
    (source / "CMakeLists.txt").write_text(cmake)
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    key = location / "disposable-probe-signing.pem"
    if not key.exists():
        data = rsa.generate_private_key(public_exponent=65537, key_size=3072).private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
        with os.fdopen(os.open(key, os.O_WRONLY|os.O_CREAT|os.O_EXCL, 0o600), "wb") as stream: stream.write(data)
    config = (ROOT / "sdkconfig").read_text()
    defaults = (PROJECT / "sdkconfig.defaults").read_text()
    for line in defaults.splitlines():
        match = re.match(r"(?:# )?CONFIG_([A-Z0-9_]+)(?:=(.*)| is not set)$", line)
        if match and (match[1].startswith("BT_") or match[1].startswith("ESP_PROTOCOMM_") or
                      match[1] == "ESP_COEX_SW_COEXIST_ENABLE"):
            config = replace_config(config, match[1], match[2] if match[2] is not None else "n")
    config = replace_config(config, "SECURE_BOOT_SIGNING_KEY", json.dumps(str(key)))
    env = idf_env(); env["IDF_COMPONENT_MANAGER"] = "0"
    profiles = [("combined-default", {}), ("wifi-flash", {
        "ESP_WIFI_IRAM_OPT":"n", "ESP_WIFI_RX_IRAM_OPT":"n",
        "ESP32_WIFI_IRAM_OPT":"n", "ESP32_WIFI_RX_IRAM_OPT":"n",
        "WIFI_RMT_IRAM_OPT":"n", "WIFI_RMT_RX_IRAM_OPT":"n",
        "BT_NIMBLE_LOW_SPEED_MODE":"y"})]
    if selected.with_controller_flash:
        profiles.append(("wifi-bt-flash", dict(profiles[-1][1], BT_CTRL_RUN_IN_FLASH_ONLY="y")))
        profiles.append(("wifi-bt-suspend", dict(profiles[-1][1], SPI_FLASH_AUTO_SUSPEND="y",
                                                SPI_FLASH_ROM_IMPL="n")))
    if selected.only: profiles=[item for item in profiles if item[0]==selected.only]
    if selected.with_transport: profiles=[(name+"-transport",changes) for name,changes in profiles]
    if selected.with_gateway: profiles=[(name+"-gateway",changes) for name,changes in profiles]
    results=[]
    for name, changes in profiles:
        print(f"Measuring isolated {name}", flush=True)
        configured=config
        for key_name,value in changes.items(): configured=replace_config(configured,key_name,value)
        config_path = location / "sdkconfig"
        config_path.write_text(configured)
        started=time.monotonic()
        execute([runtime(), SDK / "tools/idf.py", "-C", source, "-B", build,
                 "-D", f"SDKCONFIG={config_path}", "-D", "IDF_TARGET=esp32c3",
                 "-D", "PROJECT_VER=0.3.0-resource-probe", "build"], location / f"{name}-build.log", env)
        for suffix in ("elf","map","bin"):
            shutil.copy2(build / f"203-entrance.{suffix}", location / f"{name}.{suffix}")
        shutil.copy2(config_path, location / f"{name}.sdkconfig")
        execute([runtime(),"-m","esp_idf_size","--format","json2", location / f"{name}.map"], location / f"{name}-size.json",env)
        execute([runtime(),"-m","esp_idf_size","--archives","--format","json2", location / f"{name}.map"], location / f"{name}-archives.json",env)
        result=json.loads((location / f"{name}-size.json").read_text())
        result.update(profile=name,elapsed_s=round(time.monotonic()-started,2),signed_binary_bytes=(location / f"{name}.bin").stat().st_size)
        results.append(result); print(json.dumps(result),flush=True)
    summary = {"idf": actual, "production_source_snapshot": copied,
               "disposable_key": True, "flashed": False, "profiles": results}
    summary["artifact_sha256"] = {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for result in results for suffix in ("bin", "elf", "map", "sdkconfig")
        for path in [location / (result["profile"] + "." + suffix)]
    }
    rendered = json.dumps(summary, indent=2)+"\n"
    (location / ("summary-transport.json" if selected.with_transport else "summary.json")).write_text(rendered)
    if selected.only:
        (location / (results[0]["profile"]+"-summary.json")).write_text(rendered)

if __name__ == "__main__": main()
