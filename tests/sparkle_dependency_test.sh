#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEMP=$(mktemp -d); trap 'rm -rf "$TEMP"' EXIT
printf damaged > "$TEMP/bad.tar.xz"
if SPARKLE_ARCHIVE_PATH="$TEMP/bad.tar.xz" bash "$ROOT/scripts/prepare-sparkle.sh" >"$TEMP/output" 2>&1; then
    echo 'FAIL: altered dependency accepted'; exit 1
fi
PATH_TO_SPARKLE=$(bash "$ROOT/scripts/prepare-sparkle.sh")
test -L "$PATH_TO_SPARKLE/Sparkle.framework/Sparkle"
test -x "$PATH_TO_SPARKLE/bin/sign_update"
echo 'PASS: pinned dependency rejects altered bytes and preserves framework symlinks/tools'
