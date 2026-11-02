#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[[ $# == 1 ]] || exit 2
TEMP=$(mktemp -d);trap 'rm -rf "$TEMP"' EXIT
umask 077
# Public RFC 8032 test vector, not a production credential.
python3 - "$TEMP" <<'PY'
import sys,pathlib,base64
p=pathlib.Path(sys.argv[1])
p.joinpath('fixture-key').write_bytes(base64.b64encode(bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')))
PY
ditto -x -k "$1" "$TEMP/app"
python3 - "$TEMP/app/LemonVox.app/Contents/Info.plist" <<'PY'
import sys,plistlib
p=sys.argv[1];v=plistlib.load(open(p,'rb'));v['SUPublicEDKey']='11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=';v['CFBundleVersion']='10';v['CFBundleShortVersionString']='0.2.0';plistlib.dump(v,open(p,'wb'))
PY
codesign --force --sign - --timestamp=none "$TEMP/app/LemonVox.app"
ARCHIVE="$TEMP/LemonVox-0.2.0-arm64.zip"
ditto -c -k --sequesterRsrc --keepParent "$TEMP/app/LemonVox.app" "$ARCHIVE"
bash "$ROOT/scripts/prepare-update-feed.sh" "$ARCHIVE" 0.2.0 9 "$TEMP/fixture-key"
SIGNATURE=$(python3 - "$TEMP/appcast.xml" <<'PY'
import sys,xml.etree.ElementTree as E
print(E.parse(sys.argv[1]).find('./channel/item/enclosure').get('{http://www.andymatuschak.org/xml-namespaces/sparkle}edSignature'))
PY
)
xcrun swiftc -parse-as-library -module-cache-path "$TEMP/ModuleCache" "$ROOT/tests/sparkle_signature_test.swift" -o "$TEMP/verify"
"$TEMP/verify" "$ARCHIVE" '11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=' "$SIGNATURE"
if python3 "$ROOT/scripts/validate-appcast.py" --archive "$ARCHIVE" --feed "$TEMP/appcast.xml" --previous-build 10 >/dev/null 2>&1; then echo 'FAIL: non-increasing build accepted';exit 1;fi
if "$TEMP/verify" "$ARCHIVE" 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=' "$SIGNATURE"; then echo 'FAIL: wrong public key accepted';exit 1;fi
printf tampered >> "$ARCHIVE"
if "$TEMP/verify" "$ARCHIVE" '11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=' "$SIGNATURE"; then echo 'FAIL: altered archive accepted';exit 1;fi
if python3 "$ROOT/scripts/validate-appcast.py" --archive "$ARCHIVE" --feed "$TEMP/appcast.xml" --previous-build 9 >/dev/null 2>&1; then echo 'FAIL: altered archive length accepted';exit 1;fi
echo 'PASS: generated feed matches fixture archive; signatures reject tampering; build monotonicity enforced'
