#!/bin/bash
set -euo pipefail

package_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
configuration=release
build_mode=arm64
usage() { echo "Usage: build-app.sh [debug|release] [--universal]"; }
for argument in "$@"; do
    case "$argument" in
        debug|release) configuration="$argument" ;;
        --universal) build_mode=universal ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

# Packaging is offline. Require the pinned checkout instead of asking SwiftPM
# to obtain missing dependencies or consulting account credential stores.
python3 - "$package_dir" <<'PY'
import json, pathlib, subprocess, sys
package = pathlib.Path(sys.argv[1])
pins = json.loads((package / 'Package.resolved').read_text())['pins']
for pin in pins:
    checkout = package / '.build/checkouts' / pin['identity']
    if not checkout.is_dir():
        sys.exit('Pinned dependency checkout is missing. Complete the documented setup before offline packaging.')
    current = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
    changed = subprocess.check_output(['git', '-C', str(checkout), 'status', '--porcelain', '--untracked-files=all'], text=True).strip()
    if current != pin['state']['revision'] or changed:
        sys.exit('Dependency checkout differs from the pinned, unmodified revision; packaging stopped.')
PY

output_dir="$package_dir/build"
if [[ "$build_mode" == universal ]]; then output_dir="$output_dir/universal"; fi
destination="$output_dir/203 Lights.app"
ensure_not_running() {
    if [[ -f "$destination/Contents/MacOS/203Lights" ]] && /usr/sbin/lsof -t "$destination/Contents/MacOS/203Lights" >/dev/null 2>&1; then
        echo "Quit the generated 203 Lights app before replacing this build." >&2
        exit 1
    fi
}
ensure_not_running

export CLANG_MODULE_CACHE_PATH="$package_dir/.build/module-cache"
export SWIFTPM_MODULECACHE_OVERRIDE="$package_dir/.build/module-cache"
sdk_path="$(xcrun --show-sdk-path)"
swift_args=(--package-path "$package_dir" --scratch-path "$package_dir/.build" --cache-path "$package_dir/.build/cache" --config-path "$package_dir/.build/config" --security-path "$package_dir/.build/security" --force-resolved-versions --skip-update --disable-prefetching --disable-netrc --disable-keychain --disable-sandbox --sdk "$sdk_path" --configuration "$configuration")
architectures=(arm64)
if [[ "$build_mode" == universal ]]; then architectures+=(x86_64); fi
binary_dirs=()
executables=()
for build_arch in "${architectures[@]}"; do
    swift build "${swift_args[@]}" --triple "$build_arch-apple-macosx14.0" --product 203Lights
    binary_dir="$(swift build "${swift_args[@]}" --triple "$build_arch-apple-macosx14.0" --show-bin-path)"
    xcrun lipo "$binary_dir/203Lights" -verify_arch "$build_arch"
    binary_dirs+=("$binary_dir")
    executables+=("$binary_dir/203Lights")
done

mkdir -p "$output_dir"
staging="$(mktemp -d "$package_dir/build/.package.XXXXXX")"
trap 'rm -rf -- "$staging"' EXIT
archive_name="203-Lights-$build_mode-development"
distribution="$staging/$archive_name"
bundle="$distribution/203 Lights.app"
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources/Notices"
if [[ "$build_mode" == universal ]]; then
    xcrun lipo -create "${executables[@]}" -output "$bundle/Contents/MacOS/203Lights"
else
    cp "${executables[0]}" "$bundle/Contents/MacOS/203Lights"
fi
cp "$package_dir/Resources/Info.plist" "$bundle/Contents/Info.plist"
resource_name=SwiftProtobuf_SwiftProtobuf.bundle
if [[ "$build_mode" == universal ]]; then
    diff -qr "${binary_dirs[0]}/$resource_name" "${binary_dirs[1]}/$resource_name"
fi
cp -R "${binary_dirs[0]}/$resource_name" "$bundle/Contents/Resources/"
cp "$package_dir/../security/NOTICE" "$bundle/Contents/Resources/Notices/LightsSecurity-NOTICE.txt"
cp -R "$package_dir/../security/Licenses" "$bundle/Contents/Resources/Notices/"
cp "$package_dir/../security/UPSTREAM.json" "$package_dir/../security/UPSTREAM.md" "$bundle/Contents/Resources/Notices/"
cp "$package_dir/../core/LICENSE-pywizlight.txt" "$bundle/Contents/Resources/Notices/"
cp "$package_dir/.build/checkouts/swift-protobuf/LICENSE.txt" "$bundle/Contents/Resources/Notices/SwiftProtobuf-LICENSE.txt"
plutil -lint "$bundle/Contents/Info.plist"
# Ad-hoc signing is for development, not Developer ID distribution.
codesign --force --sign - "$bundle"
codesign --verify --strict --all-architectures "$bundle"

python3 - "$distribution" "$configuration" "$build_mode" "${architectures[@]}" <<'PY'
import hashlib, json, pathlib, subprocess, sys
root = pathlib.Path(sys.argv[1])
configuration, mode, expected = sys.argv[2], sys.argv[3], sorted(sys.argv[4:])
binary = root / '203 Lights.app/Contents/MacOS/203Lights'
def output(*args):
    return subprocess.check_output(args, text=True).strip()
actual = sorted(output('xcrun', 'lipo', '-archs', str(binary)).split())
if actual != expected:
    sys.exit('Packaged executable architecture set differs from the requested set.')
build_details = output('xcrun', 'vtool', '-show-build', str(binary)).replace(str(binary), binary.relative_to(root).as_posix())
minimum_versions = [line.split()[-1] for line in build_details.splitlines() if line.strip().startswith('minos ')]
if len(minimum_versions) != len(expected) or any(version not in ('14.0', '14.0.0') for version in minimum_versions):
    sys.exit('Packaged executable has an unexpected deployment target.')
manifest = {
    'format': 1, 'product': '203 Lights', 'configuration': configuration,
    'build_mode': mode, 'architectures': actual, 'minimum_macos': '14.0',
    'signature': 'ad-hoc', 'developer_id_signed': False, 'notarized': False,
    'runtime_validation': 'Neither architecture was launched by packaging; Intel runtime remains unverified.',
    'sdk_version': output('xcrun', '--show-sdk-version'),
    'swift_version': output('xcrun', 'swiftc', '--version'),
    'executable_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    'mach_o_build_details': build_details,
}
(root / 'BUILD-MANIFEST.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
(root / 'READ-ME-FIRST.txt').write_text('''203 Lights — development package

This app is ad-hoc signed. It is NOT Developer ID signed or notarized, and is
not a general-distribution release. macOS may block an untrusted download.
No security settings are disabled by this package.

Requires macOS 14 or later. BUILD-MANIFEST.json records the exact architectures,
deployment target and executable hash. A universal binary contains both Apple
Silicon and Intel code; cross-compilation is not proof of Intel runtime behavior.

The app starts disconnected. Preview is explicit; packaging does not launch
the app, access credentials, connect to a gateway, or operate any lights.
Standalone bulb provisioning and physical hardware qualification are incomplete.

Third-party notices and complete license texts are inside:
203 Lights.app/Contents/Resources/Notices/

SHA256SUMS.txt covers every other regular file in this package. A separate
.zip.sha256 file beside the archive records the SHA-256 of the complete ZIP.
''')
entries = []
for file in sorted(root.rglob('*')):
    if file.is_symlink():
        sys.exit('Unexpected symbolic link in the development package.')
    if file.is_file():
        entries.append(f'{hashlib.sha256(file.read_bytes()).hexdigest()}  {file.relative_to(root).as_posix()}')
(root / 'SHA256SUMS.txt').write_text('\n'.join(entries) + '\n')
PY

archive="$staging/$archive_name.zip"
ditto -c -k --keepParent "$distribution" "$archive"
unzip -tq "$archive"
verification="$staging/verify"
ditto -x -k "$archive" "$verification"
codesign --verify --strict --all-architectures "$verification/$archive_name/203 Lights.app"
python3 - "$verification/$archive_name" <<'PY'
import hashlib, pathlib, sys
root = pathlib.Path(sys.argv[1])
recorded = set()
for line in (root / 'SHA256SUMS.txt').read_text().splitlines():
    digest, relative = line.split('  ', 1)
    file = root / relative
    if file.is_symlink() or not file.is_file() or hashlib.sha256(file.read_bytes()).hexdigest() != digest:
        sys.exit('Archive round-trip hash verification failed.')
    recorded.add(relative)
actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
if actual != recorded | {'SHA256SUMS.txt'}:
    sys.exit('Archive round-trip contains unexpected or missing files.')
PY

ensure_not_running
if [[ -e "$destination" ]]; then
    old_bundle="$output_dir/203 Lights.previous.app"
    rm -rf -- "$old_bundle"
    mv -- "$destination" "$old_bundle"
fi
mv -- "$bundle" "$destination"
cp "$distribution/BUILD-MANIFEST.json" "$output_dir/BUILD-MANIFEST.json"
mv -- "$archive" "$output_dir/$archive_name.zip"
python3 - "$output_dir/$archive_name.zip" <<'PY'
import hashlib, pathlib, sys
archive = pathlib.Path(sys.argv[1])
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n')
print(f'ZIP SHA-256: {digest}')
PY
echo "Built development bundle: $destination"
echo "Verified development archive: $output_dir/$archive_name.zip"
echo "Ad-hoc signed only; not notarized. No app was installed or launched."
