#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEMP="$(mktemp -d)"
trap 'rm -rf "$TEMP"' EXIT

bash "$ROOT/scripts/test-core.sh" --output "$TEMP/core-build" > "$TEMP/output"
test -f "$TEMP/core-build/liblemonvox_core.a"
ar -t "$TEMP/core-build/liblemonvox_core.a" | grep -qx 'session.o'
grep -qx 'PASS: portable core library, protocol mapper, workflow and adapter contracts' "$TEMP/output"
echo 'PASS: portable core target creates the library and runs its contract checks'
