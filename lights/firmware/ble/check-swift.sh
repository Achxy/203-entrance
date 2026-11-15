#!/bin/sh
set -eu
cd "$(dirname "$0")"
out=$(mktemp -d "${TMPDIR:-/tmp}/lights-ble-swift.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
export CLANG_MODULE_CACHE_PATH="$out/clang"
export SWIFT_MODULECACHE_PATH="$out/swift"
swiftc -parse-as-library ../../macos/Sources/LightsMenulet/BluetoothFrames.swift \
  tests/test_swift_frames.swift -o "$out/check"
"$out/check"
