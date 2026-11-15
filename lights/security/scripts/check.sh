#!/bin/bash
set -euo pipefail
SECURITY_PACKAGE="$(cd "$(dirname "$0")/.." && pwd)"
export CLANG_MODULE_CACHE_PATH="$SECURITY_PACKAGE/.build/clang"
export SWIFTPM_MODULECACHE_OVERRIDE="$SECURITY_PACKAGE/.build/swift"
exec swift run --package-path "$SECURITY_PACKAGE" --disable-sandbox \
  --scratch-path "$SECURITY_PACKAGE/.build" --force-resolved-versions \
  --cache-path "$SECURITY_PACKAGE/.build/cache" \
  --config-path "$SECURITY_PACKAGE/.build/config" \
  --security-path "$SECURITY_PACKAGE/.build/security" \
  -c release LightsSecurityChecks
