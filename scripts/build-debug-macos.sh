#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -ne 2 || "$1" != --output || "$2" != /* ]]; then
    echo 'Usage: bash scripts/build-debug-macos.sh --output ABSOLUTE_NEW_DIRECTORY' >&2
    exit 2
fi
OUT="$2"
bash "$ROOT/scripts/build.sh" --output "$OUT"
mv "$OUT/LemonVox.app" "$OUT/LemonVox Debug.app"
PLIST="$OUT/LemonVox Debug.app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleIdentifier com.itomek.lemonvox.debug' "$PLIST"
/usr/libexec/PlistBuddy -c 'Set :CFBundleName LemonVox Debug' "$PLIST"
if /usr/libexec/PlistBuddy -c 'Print :CFBundleDisplayName' "$PLIST" >/dev/null 2>&1; then
    /usr/libexec/PlistBuddy -c 'Set :CFBundleDisplayName LemonVox Debug' "$PLIST"
else
    /usr/libexec/PlistBuddy -c 'Add :CFBundleDisplayName string LemonVox Debug' "$PLIST"
fi
/usr/libexec/PlistBuddy -c 'Set :SUBundleName LemonVox Debug' "$PLIST"
/usr/libexec/PlistBuddy -c 'Delete :SUFeedURL' "$PLIST"
printf 'Debug app: %s\n' "$OUT/LemonVox Debug.app"
