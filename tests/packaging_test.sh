#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
test -f "$ROOT/scripts/package-macos.sh"
TASK_TEMP=$(mktemp -d /tmp/lemonvox-package-test.XXXXXX)
# Invalid inputs must fail before creating an output directory or invoking a build.
for taskCase in missing invalid-version invalid-build missing-identity; do
    case "$taskCase" in
      missing) taskArgs=(--output "$TASK_TEMP/$taskCase" --version 0.1.0 --build-number 2);;
      invalid-version) taskArgs=(--ad-hoc --output "$TASK_TEMP/$taskCase" --version '../bad' --build-number 2);;
      invalid-build) taskArgs=(--ad-hoc --output "$TASK_TEMP/$taskCase" --version 0.1.0 --build-number wrong);;
      missing-identity) taskArgs=(--identity 0000000000000000000000000000000000000000 --output "$TASK_TEMP/$taskCase" --version 0.1.0 --build-number 2);;
    esac
    if bash "$ROOT/scripts/package-macos.sh" "${taskArgs[@]}" > "$TASK_TEMP/$taskCase.log" 2>&1; then
        echo "FAIL: invalid case $taskCase succeeded"; exit 1
    else
        taskStatus=$?
        test "$taskStatus" = 2
    fi
    test ! -e "$TASK_TEMP/$taskCase"
done
mkdir "$TASK_TEMP/existing"; printf preserved > "$TASK_TEMP/existing/sentinel"
if bash "$ROOT/scripts/package-macos.sh" --ad-hoc --output "$TASK_TEMP/existing" --version 0.1.0 --build-number 2 >/dev/null 2>&1; then echo 'FAIL: existing output accepted'; exit 1; fi
test "$(cat "$TASK_TEMP/existing/sentinel")" = preserved
if bash "$ROOT/scripts/build.sh" --output "$TASK_TEMP/existing" >/dev/null 2>&1; then echo 'FAIL: build accepted existing output'; exit 1; fi
test "$(cat "$TASK_TEMP/existing/sentinel")" = preserved
# A supplied archive must roundtrip its signature and metadata; no execution.
if [[ $# -gt 0 ]]; then
    /usr/bin/ditto -x -k "$1" "$TASK_TEMP/unpacked"
    TASK_APP="$TASK_TEMP/unpacked/LemonVox Prototype.app"
    /usr/bin/codesign --verify --strict --verbose=2 "$TASK_APP"
    test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$TASK_APP/Contents/Info.plist")" = com.itomek.lemonvox.prototype
    test -L "$TASK_APP/Contents/Frameworks/Sparkle.framework/Sparkle"
    test -s "$TASK_APP/Contents/Resources/Sparkle-LICENSE"
    test -s "$TASK_APP/Contents/Resources/THIRD_PARTY_NOTICES.md"
    test -s "$TASK_APP/Contents/Resources/yyjson-LICENSE"
    test "$(/usr/libexec/PlistBuddy -c 'Print :SUBundleName' "$TASK_APP/Contents/Info.plist")" = LemonVox
    test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIconFile' "$TASK_APP/Contents/Info.plist")" = LemonVox
    /usr/bin/codesign --verify --deep --strict "$TASK_APP"
    /usr/libexec/PlistBuddy -c 'Print :SUPublicEDKey' "$TASK_APP/Contents/Info.plist" | /usr/bin/grep -Fx '6UhLm/9m1qTqT3LVvUUggD+MTcrny6HUWAdEHoDEShc='
    cmp "$ROOT/LICENSE" "$TASK_APP/Contents/Resources/LICENSE"
    cmp "$ROOT/THIRD_PARTY_NOTICES.md" "$TASK_APP/Contents/Resources/THIRD_PARTY_NOTICES.md"
    cmp "$ROOT/third_party/yyjson/LICENSE" "$TASK_APP/Contents/Resources/yyjson-LICENSE"
    test ! -e "$TASK_APP/Contents/Resources/LemonVox Keyboard Test.app"
    printf '\ntamper' >> "$TASK_APP/Contents/Resources/LemonVox.icns"
    if /usr/bin/codesign --verify --strict "$TASK_APP" >/dev/null 2>&1; then echo 'FAIL: tampered resource accepted'; exit 1; fi
fi
echo 'PASS: packaging rejects invalid/missing signing inputs before build; supplied archive signature/identity/licenses/fixture exclusion and tamper checks'
