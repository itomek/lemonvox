#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=2.10.1
SHA=1a856d2a9edb8e628de88121d7be04cf0e2167f8641650b78fca00b14244cf27
CACHE="$ROOT/.build/dependencies"
mkdir -p "$CACHE"
ARCHIVE="${SPARKLE_ARCHIVE_PATH:-$CACHE/Sparkle-$VERSION.tar.xz}"
if [[ ! -f "$ARCHIVE" ]]; then
    [[ -z "${SPARKLE_ARCHIVE_PATH:-}" ]] || { echo 'Specified Sparkle archive is missing' >&2; exit 1; }
    curl --fail --location --silent --show-error "https://github.com/sparkle-project/Sparkle/releases/download/$VERSION/Sparkle-$VERSION.tar.xz" -o "$ARCHIVE.download"
    mv "$ARCHIVE.download" "$ARCHIVE"
fi
printf '%s  %s\n' "$SHA" "$ARCHIVE" | shasum -a 256 -c - >&2
# Always extract verified bytes; do not reuse a modified framework cache.
DEST=$(mktemp -d "$CACHE/Sparkle-$VERSION.XXXXXX")
tar -xf "$ARCHIVE" -C "$DEST"
test -d "$DEST/Sparkle.framework"
printf '%s\n' "$DEST"
