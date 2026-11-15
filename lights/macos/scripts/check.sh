#!/bin/bash
set -euo pipefail
package_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export CLANG_MODULE_CACHE_PATH="$package_dir/.build/module-cache"
export SWIFTPM_MODULECACHE_OVERRIDE="$package_dir/.build/module-cache"
swift run --package-path "$package_dir" --scratch-path "$package_dir/.build" --cache-path "$package_dir/.build/cache" --config-path "$package_dir/.build/config" --security-path "$package_dir/.build/security" --force-resolved-versions --disable-sandbox -c release LightsMenuletChecks
