#!/usr/bin/env bash
# Test runner for HVM4
#
# Test format:
#   @main = <expression>
#   //<expected output>
#
# For multi-line expected output, use multiple // lines.
# Tests starting with _ are skipped.

set -uo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
C_BIN="$DIR/../clang/main"
C_MAIN="${C_BIN}.c"
FFI_DIR="$DIR/ffi"
ROOT_DIR="$DIR/.."

# Build C
if [ ! -f "$C_MAIN" ]; then
  echo "error: expected C entrypoint at $C_MAIN" >&2
  exit 1
fi
(cd "$DIR/../clang" && clang -O2 -o main main.c)

tmp_files=()
cleanup() {
  if [ ${#tmp_files[@]} -gt 0 ]; then
    rm -f "${tmp_files[@]}"
  fi
}
trap cleanup EXIT

shopt -s nullglob
tests=()
for f in "$DIR"/*.hvm4 "$FFI_DIR"/*.hvm4; do
  name="$(basename "$f")"
  case "$name" in
    _* ) continue ;;
    *  ) tests+=("$f") ;;
  esac
done
shopt -u nullglob

if [ ${#tests[@]} -eq 0 ]; then
  echo "no .hvm4 files found under $DIR" >&2
  exit 1
fi

run_tests() {
  local bin="$1"
  local label="$2"
  local status=0

  echo "=== Testing $label ==="
  for test_file in "${tests[@]}"; do
    name="$(basename "${test_file%.hvm4}")"

    # Extract trailing // comment lines (consecutive from end of file)
    expected=""
    nlines_expected=0
    nlines_total=0
    nocollapse=0
    extra_flags=()
    expect_prefix=""
    expect_contains=""
    while IFS= read -r line; do
      if [[ "$line" == //* ]]; then
        ((nlines_total++))
        if [[ "$line" == "//FLAGS:"* ]]; then
          flag_line="${line#//FLAGS:}"
          read -r -a more_flags <<< "$flag_line"
          extra_flags+=("${more_flags[@]}")
          continue
        fi
        if [[ "$line" == "//EXPECT_PREFIX:"* ]]; then
          expect_prefix="${line#//EXPECT_PREFIX:}"
          continue
        fi
        if [[ "$line" == "//EXPECT_CONTAINS:"* ]]; then
          expect_contains="${line#//EXPECT_CONTAINS:}"
          continue
        fi
        if [[ "$line" == '//!'* ]]; then
          nocollapse=1
          content="${line#//!}"
        else
          content="${line#//}"
        fi
        [ -n "$expected" ] && expected="${content}"$'\n'"$expected"
        [ -z "$expected" ] && expected="${content}"
        ((nlines_expected++))
      else
        break
      fi
    done < <(tail -r "$test_file" 2>/dev/null || tac "$test_file")

    # For collapse_* and enum_* tests, infer limit from expected output lines
    collapse_count=""
    if [[ "$name" == collapse_* || "$name" == enum_* ]]; then
      collapse_count="$nlines_expected"
    fi

    if [ $nlines_expected -eq 0 ] && [ -z "$expect_prefix" ] && [ -z "$expect_contains" ]; then
      echo "[FAIL] $name (missing expected result comment)" >&2
      status=1
      continue
    fi

    # Create temp file without the trailing // comment lines
    tmp="$(mktemp "${DIR}/.tmp.${name}.XXXXXX")"
    tmp_files+=("$tmp")
    total=$(wc -l < "$test_file")
    keep=$((total - nlines_total))
    head -n "$keep" "$test_file" > "$tmp"

    # Determine flags: all tests use -C by default unless //! is used
    flags=""
    if [ "$nocollapse" -eq 0 ]; then
      flags="-C"
      case "$name" in
        collapse_* | enum_* )
          [ -n "$collapse_count" ] && flags="${flags}${collapse_count}"
          ;;
      esac
    fi

    ffi_flag=""
    ffi_target=""
    if [[ "$test_file" == "$FFI_DIR/"* ]]; then
      base="${test_file%.hvm4}"
      if [ -d "$base" ]; then
        ffi_flag="--ffi-dir"
        ffi_target="$base"
        shopt -s nullglob
        c_files=("$base"/*.c)
        shopt -u nullglob
        if [ ${#c_files[@]} -eq 0 ]; then
          echo "[FAIL] $name (no .c files under $base)" >&2
          status=1
          continue
        fi
        for src in "${c_files[@]}"; do
          out="${src%.c}.dylib"
          if ! clang -dynamiclib -fPIC -I "$ROOT_DIR" -o "$out" "$src"; then
            echo "[FAIL] $name (failed to build $src)" >&2
            status=1
            continue 2
          fi
          tmp_files+=("$out")
        done
      else
        src="${base}.c"
        out="${base}.dylib"
        if [ ! -f "$src" ]; then
          echo "[FAIL] $name (missing $src)" >&2
          status=1
          continue
        fi
        if ! clang -dynamiclib -fPIC -I "$ROOT_DIR" -o "$out" "$src"; then
          echo "[FAIL] $name (failed to build $src)" >&2
          status=1
          continue
        fi
        tmp_files+=("$out")
        ffi_flag="--ffi"
        ffi_target="$out"
      fi
    fi

    cmd=("$bin" "$tmp")
    if [ -n "$flags" ]; then
      cmd+=("$flags")
    fi
    if [ ${#extra_flags[@]} -gt 0 ]; then
      cmd+=("${extra_flags[@]}")
    fi
    if [ -n "$ffi_flag" ]; then
      cmd+=("$ffi_flag" "$ffi_target")
    fi

    actual="$("${cmd[@]}" 2>&1)"

    # Strip ANSI escape codes for comparison
    actual_clean="$(echo "$actual" | sed 's/\x1b\[[0-9;]*m//g')"
    expected_clean="$(echo "$expected" | sed 's/\x1b\[[0-9;]*m//g')"

    # For PARSE_ERROR tests, just check if output starts with PARSE_ERROR
    if [ -n "$expect_prefix" ]; then
      if [[ "$actual_clean" == "$expect_prefix"* ]]; then
        echo "[PASS] $name"
      else
        echo "[FAIL] $name"
        echo "  expected prefix: $expect_prefix"
        echo "  detected: $actual_clean"
        status=1
      fi
    elif [ -n "$expect_contains" ]; then
      if [[ "$actual_clean" == *"$expect_contains"* ]]; then
        echo "[PASS] $name"
      else
        echo "[FAIL] $name"
        echo "  expected to contain: $expect_contains"
        echo "  detected: $actual_clean"
        status=1
      fi
    elif [ "$expected_clean" = "PARSE_ERROR" ]; then
      if [[ "$actual_clean" == PARSE_ERROR* ]]; then
        echo "[PASS] $name"
      else
        echo "[FAIL] $name"
        echo "  expected: PARSE_ERROR"
        echo "  detected: $actual_clean"
        status=1
      fi
    elif [ "$actual_clean" = "$expected_clean" ]; then
      echo "[PASS] $name"
    else
      echo "[FAIL] $name"
      echo "  expected: $expected"
      echo "  detected: $actual"
      status=1
    fi
  done
  echo ""
  return $status
}

run_tests "$C_BIN" "C" || exit 1

echo "All tests passed!"
exit 0
