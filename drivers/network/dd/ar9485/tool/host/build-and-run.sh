#!/usr/bin/env bash
# Build and run the AR9485 lab interpreter's host test suite.
#
# Runs the real interpreter and the real script encoder against a simulated
# target, on the build host, in about a second -- so an interpreter bug costs a
# rebuild instead of one of the two reboots the lab exists to preserve.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
AR9485=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$AR9485/../../../.." && pwd)
OUT=${1:-$HERE/labtest}

gcc -std=gnu99 -Wall -Wextra -Wno-unused-parameter -O1 -g \
    -DAR9485_LAB_HOST \
    -I"$HERE" -I"$AR9485/tool" -I"$AR9485" -I"$REPO/sdk/include" \
    -o "$OUT" \
    "$HERE/labtest.c" "$AR9485/tool/lab_script.c" "$AR9485/lab_vm.c"

echo "== assertion suite =="
"$OUT"
echo
echo "== shipped scripts =="
"$OUT" --scripts "$REPO/.claude/ar9485-lab/experiments"
