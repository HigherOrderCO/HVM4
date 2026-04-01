# HVM4 CUDA Port - Context for Continuation

## What Exists

### Files
- `cuda/hvm.cu` — GPU evaluator. Single translation unit. Shares interaction rules
  directly from `clang/` via `#include` (50+ files, zero duplication). Only CUDA-specific
  code: heap ops, `term_arity` (switch vs designated initializers), WNF evaluator
  (adapted from `clang/wnf/_.c`), kernels, and host main.
- `cuda/dump.c` — CPU-side tool. `#include "../clang/hvm.c"`, parses `.hvm` source,
  writes binary dump (HEAP + BOOK + main_id) for the GPU evaluator to load.
- `cuda/bench_tree.hvm` — Parallel benchmark: binary tree depth 16 (65536 leaves),
  each leaf does 2048 tail-recursive spins. CPU baseline: 403M interactions.
- `cuda/bench_fast.hvm` — Quick iteration benchmark: depth 1 (2 leaves), 200K spins
  per leaf. ~1.2M interactions, ~1.5s on 1 GPU thread.
- `cuda/bench_8leaf.hvm` — 8-leaf variant (depth 3, 50K spins) for multi-warp testing.

### Build (on RTX machine, `ssh rtx`)
```bash
cd ~/hvm/cuda
clang -O2 -I../clang -o dump dump.c -lpthread
nvcc -O2 -arch=sm_89 -I../clang -w -o hvm_cuda hvm.cu
./dump bench_fast.hvm bench_fast.bin
./hvm_cuda bench_fast.bin -p 0     # 1 thread
./hvm_cuda bench_fast.bin -p 1     # 2 threads
./dump bench_tree.hvm bench_tree.bin
./hvm_cuda bench_tree.bin -p 16    # 65536 threads
```

### RTX Machine
- `ssh rtx` (may need `ssh-add ~/.ssh/id_rsa` first, passphrase-protected key)
- NVIDIA RTX 4090, 24 GB VRAM, compute capability 8.9, CUDA 12.4
- AMD Ryzen 9 7900X host CPU
- Rsync: `rsync -azq /Users/v/t/dev/hvm/ rtx:~/hvm/ --exclude='.git'`

### Code Sharing Strategy
The `fn` macro is `__device__ __forceinline__` in CUDA (vs `static inline` on CPU).
`ITRS_INC` increments `s_hot[S_ITRS + threadIdx.x]` (per-thread shared memory counter).
GCC `__atomic_fetch_add` is macro'd to CUDA `atomicAdd` (used by `eql_lam.c`).
All term constructors in `clang/term/new/*.c` were made C++-compatible (compound
literals replaced with explicit `heap_set` calls). Only `term_arity` remains
CUDA-specific (switch statement vs C99 designated-initializer array).

### Architecture
- **Heap**: flat `u64` array in device global memory, pre-sliced per thread (bump alloc).
- **Book**: `u64[2^24]` read-only lookup table, `BOOK[name_id] → heap_loc`.
  REF case uses `__ldg` (texture cache) for the BOOK lookup.
- **WNF stacks**: pre-allocated in device global memory, `d_wnf_stacks + tid * d_wnf_stride`.
- **Hot state**: `d_itrs`, heap bump pointers, and max-depth tracker live in per-block
  **shared memory** (`s_hot[]`), not global arrays. ~5 cycle access vs ~200+ for global RMW.
- **Heap ops**: `heap_read`/`heap_set` are plain loads/stores (L1 write-back on SM89).
  `heap_take` is `atomicExch` with spin (required for DUP cell races between threads).

### Parallel Model (Static Path Mapping)
No work-stealing queue. Each thread is assigned a deterministic path through the result
tree based on its thread ID bits. At each binary branching point (e.g., `#P{left, right}`),
bit `d` of the thread ID selects left (0) or right (1). Owner thread (lowest-ID for that
subtree) reduces the node via `wnf_at`; others spin on the heap location until WNF result
appears. After the split depth, `__syncthreads()` forces warp reconvergence, then each
thread does sequential SNF (`seq_snf`) on its subtree.

### Kernels
- `eval_kernel` — Single-thread WNF of `@main`. Stack: `WNF_STACK_CAP = 1<<27`.
- `par_eval_kernel` — Parallel SNF with static path mapping. Takes `tree_depth` and
  `split_depth`. Allocates per-thread heap slices and stacks. CLI: `-p DEPTH`.

### Launch Policy
- **≤32 threads**: `block_size = 1`. Each thread gets its own block/warp. Prevents
  intra-warp divergence in the goto-based WNF evaluator (threads doing independent
  work in the same warp serialize completely due to independent thread scheduling on SM89).
- **>32 threads**: `block_size = 256`. After the split phase + `__syncthreads()`, threads
  converge on the same code path (e.g. all running @spin), so 32 threads per warp
  execute in lockstep. 8 warps per block for good latency hiding.

### Cold Rule Outlining
Interaction rules rarely used in the spin/tree hot path (`eql_*`, `and_*`, `or_*`,
`dsu_*`, `ddu_*`, `use_*`, most `dup_*`, etc.) are compiled as `__device__ __noinline__`
instead of `__forceinline__`. Keeps the wnf() code footprint smaller without measurably
affecting hot-path performance.

## Performance Results

### bench_tree (403M interactions)

| Threads | Old MIPS | New MIPS | Improvement |
|---------|----------|----------|-------------|
| 2       | 1.44     | 1.53     | +6%         |
| 1,024   | 207      | **335**  | **+62%**    |
| 4,096   | 869      | **1,313**| **+51%**    |
| 16,384  | 2,352    | **3,847**| **+64%**    |
| 65,536  | 2,943    | **3,971**| **+35%**    |

Peak: **3,971 MIPS** at 65536 threads, up from 2,943 (+35%).
Mid-range (1K–16K) improved 50–64% from `__syncthreads` warp reconvergence.

### bench_fast (1.2M interactions)

| Threads | MIPS  | Per-thread |
|---------|-------|------------|
| 1       | 0.77  | 0.77       |
| 2       | 1.53  | 0.77       |

### CPU comparison
- Ryzen 7900X: **77 MIPS** per thread (single-thread WNF)
- M4 Max: **184 MIPS** per thread

## Root Cause Analysis

### Why `__syncthreads` was the biggest win

The split loop causes warp divergence: owner threads call `wnf_at()` while others call
`spin_until_whnf()`. On SM89 with independent thread scheduling, diverged threads do
NOT auto-reconverge — `__syncwarp()` only syncs threads in the current convergence
group (verified: `__activemask()` returned `0x00000001` after the split). Only a
block-level `__syncthreads()` barrier forces all threads back to the same PC. Without
it, 32 threads per warp were serializing the entire @spin phase. With it, they run @spin
in lockstep at ~32× throughput per warp.

### Why per-thread rate is capped at 0.77 MIPS

The bottleneck is the interpreter loop. Each @spin step does ~21 trips through the
`enter:` label, each dispatching through a ~40-case switch. On CPU this is nearly free
(branch prediction, OOO). On a GPU in-order pipeline: ~380 cycles per dispatch × 21
= ~8000 cycles/step at 2.4 GHz → 0.77 MIPS.

Evidence: adding warps does NOT improve per-thread rate (bench_8leaf: 1→8 warps, constant
0.76 MIPS/worker). 0 register spills (138 regs). Shared memory for hot state had no
effect. Explicit `st.global.wb` stores had no effect (default is already WB on SM89).
Noinline cold rules had no effect (hot path fits in icache).

### What would move per-thread rate

Compiled execution of static book definitions via the existing `aot_try_call()` hook.
This must come from the **unified AOT compiler** — the CUDA runtime is already wired
for it. The `fn`/`heap_*`/`ITRS_INC` abstractions compile on both targets unchanged.

## Memory Layout

7:1 heap-to-stack ratio. Global memory split:
```
BOOK:    128 MB   u64[2^24]        read-only
HEAP:    ~20 GB   pre-sliced per thread, bump alloc
STACKS:  ~3 GB    per-thread WNF stacks
```
Heap allocation is per-thread bump (no contention). Stack depth on bench_tree: 3.

## Key Changes Made (this session)

1. **Shared memory hot state** — moved d_itrs, heap bump pointers, max-depth from
   global arrays to `extern __shared__ u64 s_hot[]`. Eliminates global-memory RMW
   penalty per interaction/allocation.
2. **Block-size policy** — `block_size=1` for ≤32 threads (own warp, no divergence);
   `block_size=256` for large counts (converged warps after split).
3. **`__syncthreads()` after split loop** — forces warp reconvergence. Without it,
   threads serialized inside wnf() for the entire spin phase. **This was the biggest
   single win** (50–64% at scale).
4. **Cold rule outlining** — rarely-used interaction rules as `__noinline__`.
5. **`__ldg` for BOOK** — texture cache for read-only book lookups.
6. **Cached `d_wnf_stride`** — local variable in wnf() avoids reloading from global.
7. **bench_fast.hvm** — 2-leaf benchmark for ~1s single-thread iteration.
