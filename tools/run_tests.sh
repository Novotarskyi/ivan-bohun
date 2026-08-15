#!/usr/bin/env bash
# run_tests.sh - the host test suite for the firmware's pure decision kernels.
#
# Compiles firmware/tests/test_logic.c against firmware/eth/main/logic.h using
# ESP-IDF's vendored Unity, with ASan/UBSan on, and runs it on this machine.
# No board, no idf.py, no network - seconds, not minutes.
#
#   tools/run_tests.sh
#
# Needs IDF_PATH (`. ~/esp/esp-idf/export.sh` sets it) only to locate Unity.
set -euo pipefail
cd "$(dirname "$0")/.."

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
UNITY="$IDF_PATH/components/unity/unity/src"
if [ ! -f "$UNITY/unity.c" ]; then
    echo "Unity not found at $UNITY - set IDF_PATH to an ESP-IDF checkout" >&2
    exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "compiling (Unity from $UNITY, ASan+UBSan on)"
cc -std=c99 -I "$UNITY" -c "$UNITY/unity.c" -o "$OUT/unity.o"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I "$UNITY" -I firmware/eth/main \
   -c firmware/tests/test_logic.c -o "$OUT/test_logic.o"
cc -fsanitize=address,undefined "$OUT/unity.o" "$OUT/test_logic.o" -o "$OUT/test_logic"

echo "running"
"$OUT/test_logic"
