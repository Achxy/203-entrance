#!/bin/bash
set -euo pipefail
security_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
repository_dir="$(cd -- "$security_dir/../.." && pwd)"
macos_dir="$repository_dir/lights/macos"
test_python="${LIGHTS_TEST_PYTHON:-$security_dir/.research/venv/bin/python}"
if [[ "$test_python" != /* || ! -x "$test_python" ]]; then
  printf '%s\n' 'Run lights/security/scripts/bootstrap-test-env.sh, or set LIGHTS_TEST_PYTHON to an absolute compatible Python executable.' >&2
  exit 1
fi
if [[ "$(git -C "$repository_dir/.tools/esp-idf" rev-parse HEAD)" != fff9895c82d744c7237be8847347bdd1b07c6643 ]]; then
  printf '%s\n' 'Gateway wire checks require the pinned ESP-IDF v6.1 checkout.' >&2
  exit 1
fi
"$test_python" -c 'import cryptography, google.protobuf; assert cryptography.__version__ == "46.0.5"; assert google.protobuf.__version__ == "6.33.2"'
export LIGHTS_TEST_PYTHON="$test_python"
export CLANG_MODULE_CACHE_PATH="$macos_dir/.build/module-cache"
export SWIFTPM_MODULECACHE_OVERRIDE="$macos_dir/.build/module-cache"
swift build --package-path "$macos_dir" --scratch-path "$macos_dir/.build" \
  --cache-path "$macos_dir/.build/cache" --config-path "$macos_dir/.build/config" \
  --security-path "$macos_dir/.build/security" --force-resolved-versions --disable-sandbox \
  -c release --product LightsMenuletChecks
exec "$macos_dir/.build/release/LightsMenuletChecks" --gateway-wire-checks
