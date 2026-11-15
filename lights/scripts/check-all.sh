#!/bin/bash
# Host checks only. No device, radio, deployment or production build commands.
set -euo pipefail
lights_repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$lights_repo"
lights_checks="$lights_repo/.local/lights-checks"
lights_python="${LIGHTS_CHECK_PYTHON:-$lights_repo/.tools/idf-tools/python_env/idf6.1_py3.12_env/bin/python}"
lights_jobs="${LIGHTS_CHECK_JOBS:-4}"
skip_swift=0
skip_oracle=0
passes=0
skips=0

usage() {
    cat <<'EOF'
Usage: bash lights/scripts/check-all.sh [--skip-swift] [--skip-oracle]

Runs portable C/Node/Python checks and, on macOS, native Swift checks.
Non-macOS Swift checks and unavailable optional upstream fixtures are explicit
skips. Missing required tools or a failed executed check stops the run.
LIGHTS_CHECK_PYTHON overrides the pinned Python path; LIGHTS_CHECK_JOBS defaults to 4.
The optional native wire suite runs when its prepared test Python is available.
Does not flash, provision, launch apps/browsers, use radios, or deploy services.
EOF
}
while (($#)); do
    case "$1" in
        --skip-swift) skip_swift=1 ;;
        --skip-oracle) skip_oracle=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done
case "$lights_jobs" in ''|*[!0-9]*|0) echo 'LIGHTS_CHECK_JOBS must be positive.' >&2; exit 2 ;; esac
for tool in cmake make node npm "${CC:-cc}"; do
    command -v "$tool" >/dev/null || { echo "Required tool missing: $tool" >&2; exit 1; }
done
[[ -x "$lights_python" ]] || { echo "Pinned Python missing; run project setup or set LIGHTS_CHECK_PYTHON." >&2; exit 1; }
mkdir -p "$lights_checks/logs"
if ! mkdir "$lights_checks/.run-lock" 2>/dev/null; then
    echo "Another check run holds $lights_checks/.run-lock; wait for it to finish." >&2
    exit 1
fi
trap 'rmdir "$lights_checks/.run-lock"' EXIT

run() {
    local label="$1" result
    shift
    echo "RUN  $label"
    if "$@" >"$lights_checks/logs/$label.log" 2>&1; then
        passes=$((passes + 1))
        echo "PASS $label"
    else
        result=$?
        tail -60 "$lights_checks/logs/$label.log" >&2
        echo "FAIL $label; full log: $lights_checks/logs/$label.log" >&2
        exit "$result"
    fi
}
skip() {
    skips=$((skips + 1))
    echo "SKIP $1: $2"
}

run authority make -C lights/firmware/core test "BUILD=$lights_checks/authority"
run gateway-configure cmake -S lights/firmware/gateway/tests -B "$lights_checks/gateway" -DWIZ_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
run gateway-wiz-build cmake --build "$lights_checks/gateway" --target gateway_checks wiz_checks wiz_fixture_checks --parallel "$lights_jobs"
run gateway-wiz-tests ctest --test-dir "$lights_checks/gateway" --output-on-failure
run session-parser sh lights/firmware/session/check.sh
run session-vectors "$lights_python" lights/firmware/session/check_vectors.py
run owner-access "$lights_python" -m unittest discover -s tests -p test_lights_access.py -v
run candidate-build-safety "$lights_python" -m unittest discover -s lights/tools/tests -p test_build_candidate.py -v
run ble-framing sh lights/firmware/ble/check.sh
run runtime-queue sh lights/firmware/runtime/check.sh
run display-workspace-regression "$lights_python" -m unittest tests.test_display_workspace tests.test_builtin_loop tests.test_display_config tests.test_media tests.test_arcade -v
run network-time "$lights_python" -m unittest tests.test_network_time -v

if ((skip_oracle)); then
    skip wiz-upstream-fixtures '--skip-oracle selected'
elif [[ -f .local/lights-research/pywizlight/pywizlight/tests/fake_bulb.py ]]; then
    run wiz-upstream-fixtures "$lights_python" lights/firmware/wiz/tests/oracle_fixtures.py --runner "$lights_checks/gateway/wiz/wiz_fixture_checks"
else
    skip wiz-upstream-fixtures 'optional pinned pywizlight checkout absent; no download attempted'
fi

run relay-syntax npm --prefix relay run check
run relay-tests npm --prefix relay test
run web-source-pins "$lights_python" sites/lights/src/security/tests/check_upstream.py
run web-model-tests npm --prefix sites/lights test
run web-security-tests npm --prefix sites/lights run test:security
run web-syntax npm --prefix sites/lights run check
run web-package npm --prefix sites/lights run build

if ((skip_swift)); then
    skip swift-core '--skip-swift selected'
    skip swift-security '--skip-swift selected'
    skip swift-ble-framing '--skip-swift selected'
    skip swift-menulet '--skip-swift selected'
    skip swift-probe-offline '--skip-swift selected'
    skip swift-gateway-wire '--skip-swift selected'
elif [[ "$(uname -s)" != Darwin ]]; then
    skip swift-core 'CryptoKit target requires macOS'
    skip swift-security 'CryptoKit target requires macOS'
    skip swift-ble-framing 'native macOS validation is performed on macOS'
    skip swift-menulet 'SwiftUI/AppKit/CoreBluetooth target requires macOS'
    skip swift-probe-offline 'native gateway probe requires macOS'
    skip swift-gateway-wire 'native gateway adapter requires macOS'
else
    command -v swift >/dev/null || { echo 'Required macOS tool missing: swift' >&2; exit 1; }
    run swift-core env CLANG_MODULE_CACHE_PATH="$lights_checks/swift-core/clang" \
        SWIFTPM_MODULECACHE_OVERRIDE="$lights_checks/swift-core/modules" \
        swift run --package-path lights/core --scratch-path "$lights_checks/swift-core/build" \
        --cache-path "$lights_checks/swift-core/cache" --config-path "$lights_checks/swift-core/config" \
        --security-path "$lights_checks/swift-core/security" --disable-sandbox -c release LightsCoreChecks
    run swift-security bash lights/security/scripts/check.sh
    run swift-ble-framing sh lights/firmware/ble/check-swift.sh
    run swift-menulet bash lights/macos/scripts/check.sh
    run swift-probe-offline bash lights/security/scripts/check-probe.sh
    lights_wire_python="${LIGHTS_TEST_PYTHON:-$lights_repo/lights/security/.research/venv/bin/python}"
    if [[ -x "$lights_wire_python" ]]; then
        run swift-gateway-wire env LIGHTS_TEST_PYTHON="$lights_wire_python" bash lights/security/scripts/check-gateway-wire.sh
    elif [[ -n "${LIGHTS_TEST_PYTHON:-}" ]]; then
        echo 'LIGHTS_TEST_PYTHON is set but is not executable.' >&2
        exit 1
    else
        skip swift-gateway-wire 'optional Python peer environment absent; bootstrap-test-env.sh prepares it'
    fi
fi

echo "Completed: $passes passed groups, $skips explicitly skipped groups. Logs: $lights_checks/logs"
echo 'This run did not test a physical bulb, live BLE/relay, browser UI, firmware installation, or the stability soak.'
