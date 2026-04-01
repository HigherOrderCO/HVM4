#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

NVCC="nvcc -O2 -arch=sm_89 -I../clang -w"
RUNS=${RUNS:-3}

build() {
  local name=$1; shift
  echo "  Building $name ..."
  $NVCC "$@" -o "hvm_$name" hvm.cu 2>&1
}

bench() {
  local bin=$1 flags=$2 label=$3
  local best=0
  for i in $(seq 1 $RUNS); do
    local mips
    mips=$(./"$bin" "$flags" 2>/dev/null | grep 'Perf:' | awk '{print $2}')
    if [ -z "$mips" ]; then
      echo "  $label: FAILED"
      return
    fi
    if awk "BEGIN{exit ($mips > $best) ? 0 : 1}"; then
      best=$mips
    fi
  done
  printf "  %-40s %10s MIPS\n" "$label" "$best"
}

echo "=== Lock GPU clocks ==="
sudo nvidia-smi -lgc $(nvidia-smi --query-gpu=clocks.max.graphics --format=csv,noheader,nounits) 2>/dev/null || true
echo ""

echo "=== Building dump tool ==="
clang -O2 -I../clang -o dump dump.c -lpthread
echo ""

echo "=== Preparing .bin files ==="
./dump bench_tree.hvm bench_tree.bin 2>&1
./dump bench_tree17.hvm bench_tree17.bin 2>&1
echo ""

echo "=== Building variants ==="
build "baseline"
build "prefer_l1"           -DPREFER_L1
build "min_carveout"        -DMIN_CARVEOUT
build "prefer_l1_carveout"  -DPREFER_L1 -DMIN_CARVEOUT
build "no_whnf_fp"          -DNO_WHNF_FASTPATH
build "no_maxd"             -DNO_MAX_DEPTH
build "no_whnf_fp_no_maxd"  -DNO_WHNF_FASTPATH -DNO_MAX_DEPTH
build "circ"                -DCIRCULAR_HEAP
build "circ_prefer_l1"      -DCIRCULAR_HEAP -DPREFER_L1
build "circ_min_carveout"   -DCIRCULAR_HEAP -DMIN_CARVEOUT
build "circ_no_whnf_fp"     -DCIRCULAR_HEAP -DNO_WHNF_FASTPATH
build "circ_no_maxd"        -DCIRCULAR_HEAP -DNO_MAX_DEPTH
build "split"               -DCIRCULAR_HEAP -DSPLIT_KERNEL
build "split_prefer_l1"     -DCIRCULAR_HEAP -DSPLIT_KERNEL -DPREFER_L1
echo ""

echo "================================================================"
echo "=== EXPERIMENT 1: L1 Cache Config (65K threads, standard) ==="
echo "================================================================"
bench hvm_baseline          "bench_tree.bin -p 16"           "baseline (standard)"
bench hvm_prefer_l1         "bench_tree.bin -p 16"           "+PREFER_L1"
bench hvm_min_carveout      "bench_tree.bin -p 16"           "+MIN_CARVEOUT"
bench hvm_prefer_l1_carveout "bench_tree.bin -p 16"          "+PREFER_L1 +MIN_CARVEOUT"
echo ""

echo "================================================================"
echo "=== EXPERIMENT 2: L1 Cache Config (65K, circular -r 64) ==="
echo "================================================================"
bench hvm_circ              "bench_tree.bin -p 16 -r 64"     "circular baseline"
bench hvm_circ_prefer_l1    "bench_tree.bin -p 16 -r 64"     "circular +PREFER_L1"
bench hvm_circ_min_carveout "bench_tree.bin -p 16 -r 64"     "circular +MIN_CARVEOUT"
echo ""

echo "================================================================"
echo "=== EXPERIMENT 3: Circular region size sweep (65K threads) ==="
echo "================================================================"
for r in 16 24 32 48 64 96 128 256; do
  bench hvm_circ "bench_tree.bin -p 16 -r $r" "circular -r $r"
done
echo ""

echo "================================================================"
echo "=== EXPERIMENT 4: SASS lottery — wnf() changes (65K, std) ==="
echo "================================================================"
bench hvm_baseline          "bench_tree.bin -p 16"           "baseline"
bench hvm_no_whnf_fp        "bench_tree.bin -p 16"           "no WHNF fast-path"
bench hvm_no_maxd           "bench_tree.bin -p 16"           "no max_d tracking"
bench hvm_no_whnf_fp_no_maxd "bench_tree.bin -p 16"          "no WHNF fp + no max_d"
echo ""

echo "================================================================"
echo "=== EXPERIMENT 5: SASS lottery (65K, circular -r 64) ==="
echo "================================================================"
bench hvm_circ              "bench_tree.bin -p 16 -r 64"     "circ baseline"
bench hvm_circ_no_whnf_fp   "bench_tree.bin -p 16 -r 64"     "circ no WHNF fast-path"
bench hvm_circ_no_maxd      "bench_tree.bin -p 16 -r 64"     "circ no max_d tracking"
echo ""

echo "================================================================"
echo "=== EXPERIMENT 6: Two-kernel 128K threads (circular) ==="
echo "================================================================"
bench hvm_split             "bench_tree17.bin -p 17 -r 64"   "split 131K -r 64"
bench hvm_split             "bench_tree17.bin -p 17 -r 32"   "split 131K -r 32"
bench hvm_split_prefer_l1   "bench_tree17.bin -p 17 -r 64"   "split 131K -r 64 +L1"
echo ""

echo "=== Unlock GPU clocks ==="
sudo nvidia-smi -rgc 2>/dev/null || true
echo ""
echo "Done."
