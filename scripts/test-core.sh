#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -ne 2 || "$1" != --output || "$2" != /* ]]; then
    echo 'Usage: bash scripts/test-core.sh --output ABSOLUTE_NEW_DIRECTORY' >&2
    exit 2
fi
OUT="$2"
if [[ -e "$OUT" || -L "$OUT" ]]; then
    echo 'Portable core output must be a new directory; existing paths are preserved.' >&2
    exit 2
fi

CXX="${CXX:-c++}"
AR="${AR:-ar}"
mkdir -p "$OUT"
"$CXX" -std=c++17 -Wall -Wextra -Werror -I"$ROOT/core" -c "$ROOT/core/session.cpp" -o "$OUT/session.o"
"$AR" rcs "$OUT/liblemonvox_core.a" "$OUT/session.o"
"$CXX" -std=c++17 -Wall -Wextra -Werror -I"$ROOT/core" "$ROOT/tests/session_test.cpp" "$OUT/liblemonvox_core.a" -o "$OUT/session-test"
"$OUT/session-test"
"$CXX" -std=c++17 -Wall -Wextra -Werror -I"$ROOT/core" "$ROOT/tests/contracts_test.cpp" "$OUT/liblemonvox_core.a" -o "$OUT/contracts-test"
"$OUT/contracts-test"
echo 'PASS: portable core library and adapter contracts'
