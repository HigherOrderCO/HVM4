#!/usr/bin/env bash
# test/_all_.sh
# ===============
# Unified test runner for HVM4.
# Runs interpreted and AOT-compiled modes in sequence.
#
# Test format:
#   @main = <expression>
#   //<expected output>
#
# For multi-line expected output, use multiple // lines.
# Tests starting with _ are skipped.
# Per-test CLI flags can be set with one leading directive line:
#   //!--flag1 --flag2

set -uo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
C_BIN="$ROOT_DIR/clang/main"
C_MAIN="${C_BIN}.c"
FFI_DIR="$DIR/ffi"

# Config
# ------

# CLI options
# -----------
interpreted_only=0
while [ $# -gt 0 ]; do
  case "$1" in
    -i|--interpreted-only)
      interpreted_only=1
      ;;
    -h|--help)
      cat <<'EOF'
usage: test/_all_.sh [--interpreted-only|-i]

Options:
  -i, --interpreted-only   Run interpreted tests only (skip AOT tests)
  -h, --help               Show this help message
EOF
      exit 0
      ;;
    *)
      echo "error: unknown option: $1" >&2
      echo "run test/_all_.sh --help for usage" >&2
      exit 1
      ;;
  esac
  shift
done

TEST_TIMEOUT_INTERPRETED_SECS=2
TEST_TIMEOUT_COMPILED_SECS=20
HVM_TMP_DIR="${TMPDIR:-/tmp}"
shared_flags=()

# Allow env overrides
if [ -n "${HVM_TEST_TIMEOUT_SECS:-}" ]; then
  TEST_TIMEOUT_INTERPRETED_SECS="$HVM_TEST_TIMEOUT_SECS"
  TEST_TIMEOUT_COMPILED_SECS="$HVM_TEST_TIMEOUT_SECS"
fi
if [ -n "${HVM_TEST_FLAGS:-}" ]; then
  read -r -a shared_flags <<< "$HVM_TEST_FLAGS"
fi
if [ -n "${HVM_TMPDIR:-}" ]; then
  HVM_TMP_DIR="$HVM_TMPDIR"
fi
if ! mkdir -p "$HVM_TMP_DIR"; then
  echo "error: failed to create temp directory '$HVM_TMP_DIR'" >&2
  exit 1
fi

ffi_ext=".so"
ffi_ldflags=(-shared -fPIC)
case "$(uname -s)" in
  Darwin*)
    ffi_ext=".dylib"
    ffi_ldflags=(-dynamiclib -fPIC)
    ;;
esac

# Timeout
# -------

# Runs a command with a timeout; stores output in the named variable
run_with_timeout() {
  local out_var="$1" t="$2" marker cap_file pid status
  shift 2

  marker="${HVM_TMP_DIR}/hvm4-test-runner.timeout.marker"
  cap_file="${HVM_TMP_DIR}/hvm4-test-runner.capture.log"

  rm -f "$marker"
  : > "$cap_file" || return 1
  "$@" >"$cap_file" 2>&1 & pid=$!
  ( sleep "$t"; kill -0 "$pid" 2>/dev/null || exit 0; : > "$marker"; kill -KILL "$pid" 2>/dev/null || true ) &
  wait "$pid" 2>/dev/null; status=$?
  [ -f "$marker" ] && status=124
  printf -v "$out_var" '%s' "$(cat "$cap_file")"
  rm -f "$cap_file" "$marker"
  return $status
}

# Build
# -----

# Compiles the C binary
if [ ! -f "$C_MAIN" ]; then
  echo "error: expected C entrypoint at $C_MAIN" >&2
  exit 1
fi
(cd "$ROOT_DIR/clang" && clang -O2 -o main main.c)

# Collect
# -------

# Tracks generated artifact files for cleanup
cleanup_files=()
cleanup() {
  if [ ${#cleanup_files[@]} -gt 0 ]; then
    rm -f "${cleanup_files[@]}"
  fi
}
trap cleanup EXIT

# Collect test files
shopt -s nullglob
tests=()
for f in "$DIR"/*.hvm "$FFI_DIR"/*.hvm; do
  name="$(basename "$f")"
  case "$name" in
    _* ) continue ;;
    *  ) tests+=("$f") ;;
  esac
done
shopt -u nullglob

if [ ${#tests[@]} -eq 0 ]; then
  echo "no .hvm files found under $DIR" >&2
  exit 1
fi

# Run
# ---

# Runs all collected tests against the given binary
run_tests() {
  local bin="$1"
  local label="$2"
  local timeout_secs="$3"
  shift 3
  local status=0

  echo "=== Testing $label ==="
  for test_file in "${tests[@]}"; do
    name="$(basename "${test_file%.hvm}")"

    # Read leading //!... directive lines as CLI flags
    extra_flags=()
    while IFS= read -r line; do
      if [[ "$line" == "//! "* ]]; then
        flag_line="${line#//! }"
        read -r -a more_flags <<< "$flag_line"
        extra_flags+=("${more_flags[@]}")
        continue
      fi
      if [[ "$line" == "//!-"* ]]; then
        flag_line="${line#//!}"
        read -r -a more_flags <<< "$flag_line"
        extra_flags+=("${more_flags[@]}")
        continue
      fi
      if [[ -z "$line" ]]; then
        continue
      fi
      break
    done < "$test_file"

    # Extract trailing // comment lines (consecutive from end of file)
    expected=""
    nlines_expected=0
    nocollapse=0
    expect_prefix=""
    expect_contains=""
    while IFS= read -r line; do
      if [[ "$line" == //* ]]; then
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

    # Build FFI flags if applicable
    ffi_flag=""
    ffi_target=""
    if [[ "$test_file" == "$FFI_DIR/"* ]]; then
      base="${test_file%.hvm}"
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
          out="${src%.c}${ffi_ext}"
          if ! clang "${ffi_ldflags[@]}" -I "$ROOT_DIR" -o "$out" "$src"; then
            echo "[FAIL] $name (failed to build $src)" >&2
            status=1
            continue 2
          fi
          cleanup_files+=("$out")
        done
      else
        src="${base}.c"
        out="${base}${ffi_ext}"
        if [ ! -f "$src" ]; then
          echo "[FAIL] $name (missing $src)" >&2
          status=1
          continue
        fi
        if ! clang "${ffi_ldflags[@]}" -I "$ROOT_DIR" -o "$out" "$src"; then
          echo "[FAIL] $name (failed to build $src)" >&2
          status=1
          continue
        fi
        cleanup_files+=("$out")
        ffi_flag="--ffi"
        ffi_target="$out"
      fi
    fi

    # Assemble the command
    cmd=("$bin" "$test_file")
    if [ $# -gt 0 ]; then
      cmd+=("$@")
    fi
    if [ -n "$flags" ]; then
      cmd+=("$flags")
    fi
    if [ ${#extra_flags[@]} -gt 0 ]; then
      cmd+=("${extra_flags[@]}")
    fi
    if [ -n "$ffi_flag" ]; then
      cmd+=("$ffi_flag" "$ffi_target")
    fi

    # Execute and compare
    run_with_timeout actual "$timeout_secs" "${cmd[@]}"
    cmd_status=$?
    if [ $cmd_status -eq 124 ]; then
      echo "[FAIL] $name (timeout after ${timeout_secs}s)"
      status=1
      continue
    fi

    # Strip ANSI escape codes for comparison
    actual_clean="$(echo "$actual" | sed 's/\x1b\[[0-9;]*m//g')"
    expected_clean="$(echo "$expected" | sed 's/\x1b\[[0-9;]*m//g')"

    # Compare output
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

# Main
# ----

status=0
if [ -n "${HVM_TEST_FLAGS:-}" ]; then
  run_tests "$C_BIN" "HVM (interpreted)" "$TEST_TIMEOUT_INTERPRETED_SECS" "${shared_flags[@]}"
else
  run_tests "$C_BIN" "HVM (interpreted)" "$TEST_TIMEOUT_INTERPRETED_SECS"
fi
if [ $? -ne 0 ]; then
  status=1
fi

if [ "$interpreted_only" -eq 0 ]; then
  if [ -n "${HVM_TEST_FLAGS:-}" ]; then
    run_tests "$C_BIN" "HVM (AOT)" "$TEST_TIMEOUT_COMPILED_SECS" "${shared_flags[@]}" "--as-c"
  else
    run_tests "$C_BIN" "HVM (AOT)" "$TEST_TIMEOUT_COMPILED_SECS" "--as-c"
  fi
  if [ $? -ne 0 ]; then
    status=1
  fi
fi

if [ $status -eq 0 ]; then
  echo "All tests passed!"
  exit 0
else
  exit 1
fi
