#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEMP="$(mktemp -d "${TMPDIR:-/tmp}/lemonvox-debug-wrapper.XXXXXX")"
trap 'rm -rf "$TEMP"' EXIT
mkdir -p "$TEMP/repo/scripts" "$TEMP/repo/macos"
cp "$ROOT/scripts/build-debug-macos.sh" "$TEMP/repo/scripts/"
cp "$ROOT/macos/Info.plist" "$TEMP/repo/macos/Info.plist"
cat > "$TEMP/repo/scripts/build.sh" <<'STUB'
#!/bin/bash
set -euo pipefail
[[ "$1" == --output && ! -e "$2" ]]
mkdir -p "$2/LemonVox.app/Contents"
cp "$(dirname "$0")/../macos/Info.plist" "$2/LemonVox.app/Contents/Info.plist"
STUB
bash "$TEMP/repo/scripts/build-debug-macos.sh" --output "$TEMP/output"
python3 - "$ROOT/macos/Info.plist" "$TEMP/repo/macos/Info.plist" "$TEMP/output/LemonVox Debug.app/Contents/Info.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'rb') as f: source = plistlib.load(f)
with open(sys.argv[2], 'rb') as f: copied_source = plistlib.load(f)
with open(sys.argv[3], 'rb') as f: debug = plistlib.load(f)
assert source == copied_source
assert source['CFBundleIdentifier'] == 'com.itomek.lemonvox.prototype'
expected = source.copy()
expected.update(CFBundleIdentifier='com.itomek.lemonvox.debug', CFBundleName='LemonVox Debug', CFBundleDisplayName='LemonVox Debug', SUBundleName='LemonVox Debug')
expected.pop('SUFeedURL')
assert debug == expected
print('PASS: Debug wrapper changes only its output identity and removes the update feed')
PY
