#!/bin/bash
set -euo pipefail
[[ $# -ge 3 && $# -le 4 ]] || { echo 'Usage: prepare-update-feed.sh ARCHIVE VERSION PREVIOUS_BUILD [PRIVATE_KEY_FILE]' >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCHIVE="$1"; VERSION="$2"; PREVIOUS="$3"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && "$PREVIOUS" =~ ^[0-9]+$ && -f "$ARCHIVE" ]] || exit 2
SPARKLE=$(bash "$ROOT/scripts/prepare-sparkle.sh")
TEMP=$(mktemp -d); trap 'rm -rf "$TEMP"' EXIT
cp "$ARCHIVE" "$TEMP/$(basename "$ARCHIVE")"
KEY=(--account itomek.lemonvox.sparkle)
if [[ $# == 4 ]]; then KEY=(--ed-key-file "$4"); fi
"$SPARKLE/bin/generate_appcast" "${KEY[@]}" --download-url-prefix "https://github.com/itomek/lemonvox/releases/download/v$VERSION/" --maximum-deltas 0 --maximum-versions 1 -o "$TEMP/appcast.xml" "$TEMP"
python3 "$ROOT/scripts/validate-appcast.py" --archive "$ARCHIVE" --feed "$TEMP/appcast.xml" --previous-build "$PREVIOUS"
SIGNATURE=$(python3 - "$TEMP/appcast.xml" <<'PY'
import sys,xml.etree.ElementTree as E
print(E.parse(sys.argv[1]).find('./channel/item/enclosure').get('{http://www.andymatuschak.org/xml-namespaces/sparkle}edSignature'))
PY
)
"$SPARKLE/bin/sign_update" "${KEY[@]}" --verify "$ARCHIVE" "$SIGNATURE"
cp "$TEMP/appcast.xml" "$(dirname "$ARCHIVE")/appcast.xml"
echo 'Prepared and verified update feed; no release or feed published'
