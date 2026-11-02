#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT= VERSION= BUILD_NUMBER= MODE= IDENTITY=
usage() { echo 'Usage: bash scripts/package-macos.sh --output NEW_ABSOLUTE_DIRECTORY --version X.Y.Z --build-number N (--ad-hoc | --identity EXISTING_DEVELOPER_ID_SHA1)' >&2; exit 2; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output|--version|--build-number|--identity)
            [[ $# -ge 2 ]] || usage
            case "$1" in
                --output) OUTPUT="$2";; --version) VERSION="$2";; --build-number) BUILD_NUMBER="$2";;
                --identity) [[ -z "$MODE" ]] || usage; MODE=developer-id; IDENTITY="$2";;
            esac
            shift 2;;
        --ad-hoc) [[ -z "$MODE" ]] || usage; MODE=ad-hoc; shift;;
        *) usage;;
    esac
done
[[ "$OUTPUT" == /* && ! -e "$OUTPUT" && ! -L "$OUTPUT" ]] || { echo 'Output must be a new absolute directory; existing paths are preserved.' >&2; exit 2; }
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && "$BUILD_NUMBER" =~ ^[1-9][0-9]*$ && -n "$MODE" ]] || usage
if [[ "$MODE" == developer-id ]]; then
    [[ "$IDENTITY" =~ ^[A-Fa-f0-9]{40}$ ]] || { echo 'Supply the exact SHA-1 of an existing Developer ID Application identity.' >&2; exit 2; }
    # Public identity metadata only; never export keys or create credentials.
    IDENTITIES=$(/usr/bin/security find-identity -v -p codesigning)
    if ! printf '%s\n' "$IDENTITIES" | /usr/bin/awk -v fingerprint="$IDENTITY" 'toupper($2)==toupper(fingerprint) && /"Developer ID Application:/ { found=1 } END { exit !found }'; then
        echo 'Requested Developer ID Application signing identity is unavailable. No ad-hoc fallback.' >&2; exit 2
    fi
fi
mkdir -p "$OUTPUT"
WORK=$(mktemp -d "$OUTPUT/.work.XXXXXX")
BUILD_OUT="$WORK/build"
bash "$ROOT/scripts/build.sh" --output "$BUILD_OUT"
APP="$OUTPUT/LemonVox.app"
/usr/bin/ditto "$BUILD_OUT/LemonVox.app" "$APP"
# The nested keyboard-test app is a development fixture, not a shipped component.
/usr/bin/python3 - "$APP" "$VERSION" "$BUILD_NUMBER" <<'PY'
import pathlib,plistlib,shutil,sys
app=pathlib.Path(sys.argv[1]); plist=app/'Contents/Info.plist'
with plist.open('rb') as f: info=plistlib.load(f)
assert info['CFBundleIdentifier']=='com.itomek.lemonvox.prototype'
assert info['CFBundleExecutable']=='LemonVoxPrototype'
info['CFBundleShortVersionString']=sys.argv[2]; info['CFBundleVersion']=sys.argv[3]
with plist.open('wb') as f: plistlib.dump(info,f)
fixture=app/'Contents/Resources/LemonVox Keyboard Test.app'
if fixture.exists(): shutil.rmtree(fixture)
PY
# Seal nested updater code inside-out, using the same identity as the app.
SPARKLE_FRAMEWORK="$APP/Contents/Frameworks/Sparkle.framework"
for COMPONENT in "$SPARKLE_FRAMEWORK/Versions/B/Autoupdate" "$SPARKLE_FRAMEWORK/Versions/B/XPCServices/Downloader.xpc" "$SPARKLE_FRAMEWORK/Versions/B/XPCServices/Installer.xpc" "$SPARKLE_FRAMEWORK/Versions/B/Updater.app" "$SPARKLE_FRAMEWORK"; do
    if [[ "$MODE" == developer-id ]]; then
        /usr/bin/codesign --force --sign "$IDENTITY" --options runtime --timestamp --preserve-metadata=entitlements "$COMPONENT"
    else
        /usr/bin/codesign --force --sign - --timestamp=none --preserve-metadata=entitlements "$COMPONENT"
    fi
done
if [[ "$MODE" == developer-id ]]; then
    /usr/bin/codesign --force --sign "$IDENTITY" --identifier com.itomek.lemonvox.prototype --options runtime --timestamp --entitlements "$ROOT/macos/LemonVox.entitlements" "$APP"
else
    /usr/bin/codesign --force --sign - --identifier com.itomek.lemonvox.prototype --timestamp=none "$APP"
    echo 'Local ad-hoc package: sealed resources, no certificate-backed update identity or Gatekeeper/notarization guarantee.' >&2
fi
/usr/bin/codesign --verify --strict --verbose=2 "$APP"
/usr/bin/codesign -d -r- "$APP" > "$OUTPUT/signature.txt" 2>&1
ARCH=$(/usr/bin/uname -m)
ARCHIVE="$OUTPUT/LemonVox-$VERSION-$BUILD_NUMBER-$ARCH-$MODE.zip"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$APP" "$ARCHIVE"
SOURCE_REVISION=$(git -C "$ROOT" rev-parse HEAD)
SOURCE_DIRTY=false
[[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]] || SOURCE_DIRTY=true
/usr/bin/python3 - "$OUTPUT" "$VERSION" "$BUILD_NUMBER" "$MODE" "$ARCH" "$SOURCE_REVISION" "$SOURCE_DIRTY" <<'PY'
import hashlib,json,pathlib,sys
out=pathlib.Path(sys.argv[1]); archive=next(out.glob('*.zip'))
manifest=dict(bundle_id='com.itomek.lemonvox.prototype',executable='LemonVoxPrototype',version=sys.argv[2],build=sys.argv[3],signing=sys.argv[4],architecture=sys.argv[5],source_revision=sys.argv[6],source_dirty=sys.argv[7]=='true',archive=archive.name,sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),notarized=False)
(out/'SHA256SUMS').write_text(manifest['sha256']+'  '+archive.name+'\n')
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
PY
/usr/bin/python3 - "$WORK" <<'PY'
import pathlib,shutil,sys
work=pathlib.Path(sys.argv[1]); assert work.name.startswith('.work.')
shutil.rmtree(work)
PY
printf 'Verified local package: %s\n' "$ARCHIVE"
