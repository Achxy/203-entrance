#!/bin/bash
set -euo pipefail
umask 077
if [[ $# -ne 0 ]]; then
  printf '%s\n' 'Usage: bash lights/macos/scripts/build-probe.sh' >&2
  exit 2
fi
package_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# Offline packaging uses only already-resolved, pinned dependencies. It never
# requests an account identity or alters any existing menulet distribution.
python3 - "$package_dir" <<'PY'
import json, pathlib, subprocess, sys
package = pathlib.Path(sys.argv[1])
for pin in json.loads((package / 'Package.resolved').read_text())['pins']:
    checkout = package / '.build/checkouts' / pin['identity']
    if not checkout.is_dir():
        sys.exit('Pinned dependency is absent. Complete setup before packaging.')
    revision = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
    changed = subprocess.check_output(['git', '-C', str(checkout), 'status', '--porcelain', '--untracked-files=all'], text=True).strip()
    if revision != pin['state']['revision'] or changed:
        sys.exit('Dependency differs from its pinned unmodified revision.')
PY
export CLANG_MODULE_CACHE_PATH="$package_dir/.build/module-cache"
export SWIFTPM_MODULECACHE_OVERRIDE="$package_dir/.build/module-cache"
swift_args=(--package-path "$package_dir" --scratch-path "$package_dir/.build" --cache-path "$package_dir/.build/cache" --config-path "$package_dir/.build/config" --security-path "$package_dir/.build/security" --force-resolved-versions --skip-update --disable-prefetching --disable-netrc --disable-keychain --disable-sandbox --configuration release)
swift build "${swift_args[@]}" --product LightsGatewayProbe
binary_dir="$(swift build "${swift_args[@]}" --show-bin-path)"
if [[ -L "$package_dir/build" || -L "$package_dir/build/probe" ]]; then
  printf '%s\n' 'Refusing a symbolic-link probe output directory.' >&2
  exit 1
fi
mkdir -p "$package_dir/build/probe"
staging="$(mktemp -d "$package_dir/build/probe/qualification.XXXXXX")"
trap 'rm -rf -- "$staging"' EXIT
bundle="$staging/203 Lights Probe.app"
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources/Notices"
cp "$binary_dir/LightsGatewayProbe" "$bundle/Contents/MacOS/LightsGatewayProbe"
cp "$package_dir/ProbeResources/Info.plist" "$bundle/Contents/Info.plist"
cp -R "$binary_dir/SwiftProtobuf_SwiftProtobuf.bundle" "$bundle/Contents/Resources/"
cp "$package_dir/../security/NOTICE" "$bundle/Contents/Resources/Notices/LightsSecurity-NOTICE.txt"
cp -R "$package_dir/../security/Licenses" "$bundle/Contents/Resources/Notices/"
cp "$package_dir/../core/LICENSE-pywizlight.txt" "$bundle/Contents/Resources/Notices/"
cp "$package_dir/.build/checkouts/swift-protobuf/LICENSE.txt" "$bundle/Contents/Resources/Notices/SwiftProtobuf-LICENSE.txt"
plutil -lint "$bundle/Contents/Info.plist"
codesign --force --sign - --identifier dev.achyuth.203lights.probe "$bundle"
codesign --verify --strict --all-architectures "$bundle"
# --check does not instantiate a Bluetooth manager, query TCC, open a socket,
# load a real owner bundle, or invoke Launch Services.
"$bundle/Contents/MacOS/LightsGatewayProbe" --check
python3 - "$bundle" "$staging" <<'PY'
import hashlib, json, pathlib, platform, plistlib, subprocess, sys
bundle, output = map(pathlib.Path, sys.argv[1:])
info = plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())
assert info['CFBundleIdentifier'] == 'dev.achyuth.203lights.probe'
assert info['CFBundleExecutable'] == 'LightsGatewayProbe'
assert info['NSBluetoothAlwaysUsageDescription']
binary = bundle / 'Contents/MacOS/LightsGatewayProbe'
manifest = {'v': 1, 'bundle': str(bundle), 'bundle_id': info['CFBundleIdentifier'],
            'executable_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'host_architecture': platform.machine(), 'signing': 'ad_hoc_development',
            'designated_requirement': subprocess.check_output(['codesign', '--display', '-r', '-', str(bundle)], stderr=subprocess.STDOUT, text=True).strip(),
            'permission_requested': False, 'radio_accessed': False, 'hardware_qualified': False,
            'notarized': False, 'existing_menulet_replaced': False}
with (output / 'manifest.json').open('x') as stream:
    json.dump(manifest, stream, indent=2); stream.write('\n')
print(json.dumps(manifest, indent=2))
PY
trap - EXIT
