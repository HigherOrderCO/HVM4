#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CC="${CC:-clang}"
LLVM_PROFDATA_BIN="${LLVM_PROFDATA:-}"

if [[ -z "$LLVM_PROFDATA_BIN" ]]; then
  if command -v llvm-profdata >/dev/null 2>&1; then
    LLVM_PROFDATA_BIN="$(command -v llvm-profdata)"
  else
    LLVM_PROFDATA_BIN="$(xcrun -f llvm-profdata)"
  fi
fi

PROF_DIR="${TMPDIR:-/tmp}/hvm-pgo-profile.$$"
GEN_BIN="$PROF_DIR/hvm-pgo-gen"
PROF_DATA="$PROF_DIR/hvm.profdata"
OUT_BIN="$ROOT_DIR/src/hvm"
CFLAGS=(${CFLAGS:-}
  -O3
  -std=c11
  -Wall
  -Wextra
  -Wno-unused-function
  -Wno-unused-parameter
)

cleanup() {
  rm -rf "$PROF_DIR"
}
trap cleanup EXIT

mkdir -p "$PROF_DIR"

"$CC" "${CFLAGS[@]}" -fprofile-generate="$PROF_DIR" -o "$GEN_BIN" "$ROOT_DIR/src/hvm.c"

for file in \
  "$ROOT_DIR/devs/bench/nano_bitlist_count_ones.hvm" \
  "$ROOT_DIR/devs/bench/nano_bitlist_decrement.hvm" \
  "$ROOT_DIR/devs/bench/nano_bitlist_map_not.hvm" \
  "$ROOT_DIR/devs/bench/nano_nat_addition.hvm" \
  "$ROOT_DIR/devs/bench/nano_nat_multiplication.hvm" \
  "$ROOT_DIR/devs/bench/nano_natlist_radix_sort.hvm" \
  "$ROOT_DIR/devs/bench/nano_natlist_reverse.hvm" \
  "$ROOT_DIR/devs/bench/nano_tupletree_mirror.hvm" \
  "$ROOT_DIR/devs/bench/cnot_24.hvm"; do
  "$GEN_BIN" "$file" -s -S >/dev/null
done

"$LLVM_PROFDATA_BIN" merge -output="$PROF_DATA" "$PROF_DIR"/*.profraw
"$CC" "${CFLAGS[@]}" -fprofile-use="$PROF_DATA" -o "$OUT_BIN" "$ROOT_DIR/src/hvm.c"
