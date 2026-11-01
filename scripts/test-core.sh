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
CC="${CC:-cc}"
AR="${AR:-ar}"
mkdir -p "$OUT"
"$CC" -std=c17 -Wall -Wextra -Werror -DYYJSON_READER_DEPTH_LIMIT=513 -I"$ROOT/third_party/yyjson" -c "$ROOT/third_party/yyjson/yyjson.c" -o "$OUT/yyjson.o"
"$CXX" -std=c++17 -Wall -Wextra -Werror -I"$ROOT/core" -c "$ROOT/core/session.cpp" -o "$OUT/session.o"
"$CXX" -std=c++17 -Wall -Wextra -Werror -DYYJSON_READER_DEPTH_LIMIT=513 -I"$ROOT/core" -I"$ROOT/third_party/yyjson" -c "$ROOT/core/protocol.cpp" -o "$OUT/protocol.o"
"$CXX" -std=c++17 -Wall -Wextra -Werror -DYYJSON_READER_DEPTH_LIMIT=513 -I"$ROOT/core" -I"$ROOT/third_party/yyjson" -c "$ROOT/core/workflow.cpp" -o "$OUT/workflow.o"
"$AR" rcs "$OUT/liblemonvox_core.a" "$OUT/session.o" "$OUT/protocol.o" "$OUT/workflow.o" "$OUT/yyjson.o"
"$CXX" -std=c++17 -Wall -Wextra -Werror -I"$ROOT/core" "$ROOT/tests/session_test.cpp" "$OUT/liblemonvox_core.a" -o "$OUT/session-test"
"$OUT/session-test"
"$CXX" -std=c++17 -Wall -Wextra -Werror -I"$ROOT/core" "$ROOT/tests/trigger_core_test.cpp" "$OUT/liblemonvox_core.a" -o "$OUT/trigger-core-test"
"$OUT/trigger-core-test"
"$CXX" -std=c++17 -Wall -Wextra -Werror -I"$ROOT/core" "$ROOT/tests/contracts_test.cpp" "$OUT/liblemonvox_core.a" -o "$OUT/contracts-test"
"$OUT/contracts-test"
"$CXX" -std=c++17 -Wall -Wextra -Werror -DYYJSON_READER_DEPTH_LIMIT=513 -I"$ROOT/core" -I"$ROOT/third_party/yyjson" "$ROOT/tests/protocol_test.cpp" "$OUT/liblemonvox_core.a" -o "$OUT/protocol-test"
"$OUT/protocol-test"
"$CXX" -std=c++17 -Wall -Wextra -Werror -I"$ROOT/core" "$ROOT/tests/workflow_core_test.cpp" "$OUT/liblemonvox_core.a" -o "$OUT/workflow-core-test"
"$OUT/workflow-core-test"
echo 'PASS: portable core library, protocol mapper, workflow and adapter contracts'
