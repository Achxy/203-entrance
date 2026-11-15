#!/bin/bash
set -euo pipefail
security_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
macos_dir="$(cd -- "$security_dir/../macos" && pwd)"
export CLANG_MODULE_CACHE_PATH="$macos_dir/.build/module-cache"
export SWIFTPM_MODULECACHE_OVERRIDE="$macos_dir/.build/module-cache"
exec swift run --package-path "$macos_dir" --scratch-path "$macos_dir/.build" \
  --cache-path "$macos_dir/.build/cache" --config-path "$macos_dir/.build/config" \
  --security-path "$macos_dir/.build/security" --force-resolved-versions --disable-sandbox \
  -c release LightsGatewayProbe --check
