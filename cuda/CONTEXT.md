# HVM4 CUDA Port - Context for Continuation

## What Exists

### Files
- `cuda/hvm.cu` — GPU evaluator (~1500 lines). Shares interaction rules from `clang/` via
  `#include`. Contains: compatibility layer, heap/stack ops, WNF evaluator, two kernels
  (single-thread `eval_kernel`, parallel `par_eval_kernel`), and host main.
- `cuda/dump.c` — CPU-side parser → binary dump (HEAP + BOOK + main_id). Includes
  `../clang/hvm.c` for the full CPU runtime, then serializes the parsed book.
- `cuda/bench_tree.hvm` — depth 16, 65536 leaves, seq 2048. 403M interactions.
- `cuda/bench_fast.hvm` — depth 1, 2 leaves, seq 200K. ~1.2M interactions, ~1.4s/1 GPU thread.
- `cuda/bench_8leaf.hvm` — depth 3, 8 leaves, seq 50K.

### Build (`ssh rtx`)
```bash
cd ~/hvm/cuda
clang -O2 -I../clang -o dump dump.c -lpthread
nvcc -O2 -arch=sm_89 -I../clang -w -o hvm_cuda hvm.cu
./dump bench_fast.hvm bench_fast.bin
./hvm_cuda bench_fast.bin -p 1     # 2 threads
./hvm_cuda bench_tree.bin -p 16    # 65536 threads

# Circular heap build (for benchmarks where split_depth == tree_depth):
nvcc -O2 -arch=sm_89 -I../clang -w -DCIRCULAR_HEAP -o hvm_cuda_circ hvm.cu
./hvm_cuda_circ bench_tree.bin -p 16 -r 64   # 64 KB region per warp
```

### RTX Machine
- `ssh rtx` (may need `ssh-add ~/.ssh/id_rsa`)
- RTX 4090, 24 GB VRAM, SM 8.9 (Ada Lovelace), CUDA 12.4, Ryzen 7900X
- 128 SMs, 1 TB/s DRAM, 72 MB L2 cache, 128 KB L1 per SM, 65536 regs per SM
- Rsync: `rsync -azq /Users/v/t/dev/hvm/ rtx:~/hvm/ --exclude='.git'`

## Current Performance

| Threads | Standard MIPS | Circular MIPS | Change   |
|---------|---------------|---------------|----------|
| 1       | 0.85          | —             | —        |
| 1,024   | 682           | —             | —        |
| 16,384  | 10,661        | —             | —        |
| 32,768  | 18,749        | —             | —        |
| 65,536  | **22,713**    | **23,620**    | **+4.0%**|

CPU baselines: Ryzen 77 MIPS/thread, M4 Max 184 MIPS/thread.

**23.6 GIPS peak at 65K threads with circular heap (`-DCIRCULAR_HEAP -r 64`).** Standard
build peaks at 22.7 GIPS. Per-thread scaling efficiency at 65K: 43% (circular) / 41%
(standard). Theoretical max: 0.85 × 65536 = 55.7 GIPS.

### Register / Occupancy Profile
- `par_eval_kernel` (standard): 60 registers, 384 bytes stack, 0 spills → 8 blocks/SM.
- `par_eval_kernel` (circular): 61 registers, 384 bytes stack, 0 spills → 8 blocks/SM.
- `eval_kernel`: 46 registers, 128 bytes stack, 0 spills.
- `heap_alloc_coop`: noinline, ~20 registers.

### Nsight Compute Profile (65K threads, bench_tree, standard build)
- DRAM reads: 61 MB (near-zero — reads fully cached in L2).
- DRAM writes: 11.8 GB (dirty line evictions from heap allocations).
- L1 load hit rate: 84.4%.
- Active warps: 31.24% of peak (16 warps/SM, limited by thread count not registers).
- Instructions: 32.2 per interaction.
- Effective DRAM write bandwidth: ~655 GB/s (~65% of peak 1 TB/s).
- **Bottleneck**: occupancy (31%), not bandwidth. Only 16 warps/SM = 4 warps per scheduler.
  Can't fully hide L1 memory latency (~28 cycles) with 4 warps and ~5 instructions between
  memory ops.

## Circular Heap Allocation

Compile-time feature (`-DCIRCULAR_HEAP`). Caps per-warp heap region to a fixed size and
wraps the bump pointer when it reaches the region end. Improves L2 locality by confining
the working set to a small address range instead of spreading across 10 MB per warp.

**How it works**: shared memory `S_WARP_BASE` stores each warp's region start. When
`S_WARP_HEAP` exceeds `S_WARP_END`, the leader resets it to `S_WARP_BASE`. All allocations
within a warp remain contiguous (coalesced).

**Safety constraint**: only safe when each thread's entire reduction stays within the
circular region's safety margin. For @spin-like workloads: alive data is from the last 2-3
steps (~10 KB), so a 64 KB region (18 steps before wrap) has ~6x safety margin.

**Limitation**: must use `split_depth == tree_depth` (1 leaf per thread). With deeper splits
or multi-leaf-per-thread workloads, tree nodes allocated during the split phase can be
overwritten before other warps read them. A global barrier after the split would fix this
but isn't implemented yet.

## Code Architecture (hvm.cu layout, top-to-bottom)

### Compatibility Layer (lines 1–30)
- `#define fn __device__ __forceinline__` — all included CPU rules use `fn`.
- `ITRS_INC(name)` → `d_itrs++` (shared memory per-thread counter).
- `__atomic_fetch_add` mapped to `atomicAdd` for eql_lam.c compatibility.

### Tags + WHNF_MASK (lines 50–110)
- 46 tag constants (APP=0 through PRI=45).
- `WHNF_MASK`: 64-bit bitmask of tags that are already in weak head normal form. Used by
  `is_whnf_tag()`, `wnf_at()`, `spin_until_whnf()`, and VAR/DP fast-paths in wnf().

### Shared Memory Layout (lines 170–188)
```
s_hot[0..255]   = S_ITRS       per-thread interaction counter
s_hot[256..511] = S_MAX_DEPTH   per-thread max stack depth
s_hot[512..519] = S_WARP_HEAP   per-warp heap bump pointer (next)
s_hot[520..527] = S_WARP_END    per-warp heap region end
s_hot[528..535] = S_WARP_BASE   per-warp heap region start (for circular wrap)
Total: 536 u64 = 4288 bytes per block.
```
- `d_itrs` macro = `s_hot[S_ITRS + threadIdx.x]`.
- `c_arity[48]`: device-const arity lookup table, replaces 40-case switch.

### Heap Allocator (lines 209–239)
Two-tier design:
1. **`heap_alloc(size)`** (forceinline): calls `__activemask()`, checks `__popc(mask)`.
   If count==1 (single active thread): simple shared-memory bump. Otherwise → coop path.
   With `CIRCULAR_HEAP`: both paths check for region wrap.
2. **`heap_alloc_coop(size)`** (noinline): warp-cooperative allocation.
   - Gets fresh `__activemask()` inside (critical for SM89 independent thread scheduling —
     passing a stale mask from the caller causes illegal memory access).
   - Uniform-size fast path: leader bumps per-warp pointer by `count * size`, broadcasts
     base via `__shfl_sync`, each thread gets `base + rank * size`. All warp allocations
     are contiguous → writes coalesce into ~4 cache lines instead of 32.
   - With `CIRCULAR_HEAP`: leader checks if bump exceeds `S_WARP_END`; if so, resets to
     `S_WARP_BASE`.
   - Non-uniform fallback: per-thread `atomicAdd` on shared memory (correct but not coalesced).

**Critical correctness note**: `heap_alloc_coop` MUST call `__activemask()` itself, not
receive it as a parameter. On SM89, the mask can become stale between an inline caller's
`__activemask()` and the noinline callee's `__shfl_sync()`, causing threads named in the mask
to not actually be present at the sync point → undefined behavior / illegal memory access.

### heap_read / heap_set / heap_take (lines 241–258)
- `heap_read(loc)`: plain `HEAP[loc]` load.
- `heap_set(loc, val)`: plain `HEAP[loc] = val` store.
- `heap_take(loc)`: `atomicExch` spin loop (for DUP cell races).
- `heap_set_rel(loc, val)`: volatile store (for substitution visibility across threads).

### Term Operations (lines 260–320)
Included from `clang/`: `term/new.c`, `term/tag.c`, `term/ext.c`, `term/val.c`,
`term/sub/{get,set}.c`, `term/op2_u32.c`, `term/new/*.c` (all constructors),
`term/clone.c`, `heap/subst_var.c`, `heap/subst_var_dup.c`, `heap/subst_cop.c`.

`term_arity(t)` → `c_arity[term_tag(t)]` (table lookup, not a switch).

### Interaction Rules (lines 320–382)
**Hot rules** (`fn` = forceinline, inlined into wnf):
`app_era`, `app_lam`, `app_mat_num`, `alo_var`, `alo_lam`, `alo_nod`,
`op2_num_num`, `dup_sup`, `dup_nod`.

**Cold rules** (redefined `fn` = `__device__ __noinline__`):
All others (~50 rules). Keeps wnf() code size small for icache.

### WNF Evaluator (lines 384–1092)
`__device__ __noinline__ Term wnf(Term term)`:
- **d_wnf_stride**: stores `n_threads` (used as stride for depth-major stack layout).
- **Stack**: depth-major interleaved via `PUSH(v)` / `POP()` macros:
  ```
  PUSH(v): d_wnf_stacks[(u64)s_pos * nt + tid] = v; s_pos++
  POP():   --s_pos; d_wnf_stacks[(u64)s_pos * nt + tid]
  ```
  Consecutive threads access consecutive addresses → warp coalescing (2 cache lines per
  push instead of 32).
- **max_depth**: register-local `max_d`, updated only on push, flushed to s_hot at return
  and early-return points (APP→NUM, APP→CTR).
- **WHNF fast-path**: in VAR and DP0/DP1 enter cases, after resolving a substitution, checks
  `(WHNF_MASK >> term_tag(next)) & 1`. If true, jumps directly to `apply` without
  re-entering the switch. Saves ~380 cycles per instance (~3 per @spin step).

### wnf_at + is_whnf_tag (lines 1094–1104)
- `is_whnf_tag(tag)`: `(WHNF_MASK >> tag) & 1`. Single-instruction check.
- `wnf_at(loc)`: reads cell, short-circuits if WHNF, else calls wnf() and writes back.

### eval_kernel (lines 1110–1130)
Single-thread WNF evaluation. Sets `d_wnf_stride = 1`. Initializes one warp heap region
covering the full heap. Returns result term + itrs + max_depth + heap_next.

### par_eval_kernel (lines 1164–1210)
Parallel SNF evaluation via static binary-tree split:
1. **Heap init**: per-warp regions. `tpr` (threads per region) = 32 if block_size≥32, else 1.
   `n_regions = n_threads / tpr`. Each region gets `heap_size / n_regions` words (optionally
   capped by `max_region` for circular allocation).
   `__syncthreads()` after init (needed because different warps' lane-0 threads init their
   warp's pointers and all threads must see them).
2. **Split loop**: for depth 0..split_depth-1, owner thread (lowest matching bits) calls
   `wnf_at(loc)` + `__threadfence()`, others `spin_until_whnf(loc)` with nanosleep after
   64 polls. Each thread follows its bit path down the tree.
3. **`__syncwarp()`** after split for reconvergence.
4. **`seq_snf(loc)`**: iterative SNF with 32-entry work stack. Calls wnf_at on each subterm.
5. Results: `atomicAdd` for total itrs, `atomicMax` for max depth.

### Host Code (lines 1220–1490)
- `run_eval()`: single-thread mode. Allocates 6 GB heap, 128 MB book, 1 GB WNF stack.
- `run_par_eval()`: parallel mode. Block size: 1 for ≤32 threads, 128 for >32.
  Stack: 1024 entries/thread for ≤65K, 64 for >65K (interleaved). Heap: fills available
  VRAM minus overhead.
- `-p DEPTH` flag: `n_threads = 1 << depth`, `split_depth = depth`.
- `-r REGION_KB` flag: caps per-warp heap region to REGION_KB × 1024 bytes (circular mode).

## Why Per-Thread Rate Drops at Scale

### The @spin Hot Path (per step, ~6 interactions, ~21 WNF dispatches)
1. REF(@spin) → BOOK lookup (__ldg) → ALO
2. ALO → wnf_alo_lam: heap_alloc(2), heap_alloc(1), 3 heap_set → LAM
3. APP + LAM → wnf_app_lam: heap_read, heap_subst_var → body (ALO wrapping MAT)
4. ALO → wnf_alo_nod: heap_alloc(2), 2 heap_set → MAT{ALO, ALO}
5. MAT + NUM → wnf_app_mat_num: 2 heap_read, 2 heap_set (reuse loc) → APP(else, num)
6. OP2 + NUM + NUM → wnf_op2_num_num: pure ALU → NUM

~20 heap accesses per step, ~8 heap_alloc calls per step (including term_new_alo calls).

### Bottleneck Analysis (65K threads)
Primary bottleneck is **occupancy** (31%), not DRAM bandwidth (65% utilized):
- 65K threads / 128 SMs = 512 threads/SM = 16 warps/SM
- 4 warp schedulers per SM → 4 warps per scheduler
- With ~5 instructions between memory ops and ~28-cycle L1 latency:
  4 warps hide only 20 cycles, leaving 8 cycles exposed per memory access
- DRAM write traffic: 11.8 GB (dirty evictions), ~655 GB/s → 65% of peak
- DRAM read traffic: 61 MB → near zero (fully cached in L2)
- More threads would help but the benchmark has exactly 65K leaves

### Memory Liveness in @spin
Per step: ~14 words allocated, but only last 2-3 steps' data (~10 KB/warp) is alive.
Over 2048 steps per leaf: total allocation is 7 MB/warp, of which 99.8% is dead.
With linear allocation, dead data occupies L2 cache lines until evicted. Circular
allocation reclaims dead space, confining the working set and reducing eviction traffic.

## What Was Tried and Didn't Help (or Marginal)

- **`__launch_bounds__(128, 4)`** — forced 128 regs, 4 blocks/SM. Helped 65K (+10%) but hurt
  32K (-7%) due to 156-byte spills. Removed; noinline heap_alloc_coop achieves 60 regs
  without spills.
- **`__launch_bounds__(128, 3)`** — 168 regs, spills, worse at 65K. Discarded.
- **`st.global.wb` explicit stores** — default is already WB on SM89, no change.
- **Noinline cold rules for icache** — hot path already fits, no measurable change (kept for
  register pressure benefit).
- **Eager ALO (recursive copy replacing lazy ALO)** — stack overflow issues, 2T regressed.
- **Warp-stride (same-SM placement for 2T)** — no per-thread improvement, reverted.
- **Register-based ITRS counter** — incompatible with noinline cold rules (they can't access
  caller's registers). Shared memory overhead is <1% so not worth the complexity.
- **Passing `mask` parameter to noinline heap_alloc_coop** — caused illegal memory access on
  SM89. The mask from `__activemask()` in the inline caller goes stale before the noinline
  callee's `__shfl_sync`. Fixed by getting fresh mask inside the noinline function.
- **Register stack cache (4 entries)** — tried sc[4] array (went to local memory = DRAM, 50%
  regression at 65K) and named registers sc0-sc3 (70 regs, icache pressure, 48% regression).
  The stack already hits L1 (84% hit rate) so register caching saves at most 28 cycles/access.
  Not worth the register pressure / icache cost on this GPU.
- **WHNF fast-paths in enter cases (APP, DUP, OP2, etc.)** — added `(WHNF_MASK >> tag) & 1`
  check after each heap_read. Increased registers from 60 → 70 (+10!), causing 48% regression
  at 65K. The wnf() function's compiler-generated code is extremely sensitive to control-flow
  changes — even small additions cascade into register allocation changes.
- **ALO dispatch shortcuts (WHNF results → goto apply)** — sending ALO-LAM, ALO-NOD(MAT),
  etc. directly to `goto apply` instead of `goto enter`. Improved 1T-32K by 5-7%, but caused
  48% regression at 65K. Same compiler sensitivity issue. The SASS layout changes unpredictably.
- **`__ldg` for book term reads in ALO** — neutral effect. Book data is immutable but the
  L1/texture cache is unified on SM89, so `__ldg` doesn't help.
- **`-O3` flag** — worse than `-O2` at all thread counts. Device code optimization is controlled
  by ptxas, not nvcc optimization level.
- **`--maxrregcount=48-56`** — small spills, marginal changes (±1%). At 52 regs with 48-byte
  spills: +1.1%. Not worth the fragility.
- **Vectorized 128-bit stores** — requires 16-byte alignment. Mixed 1-word and 2-word
  allocations make alignment unpredictable. Padding wastes 50% of 1-word allocs.

## What Would Further Improve Performance

1. **Higher occupancy via more leaves** — the 65K benchmark is occupancy-limited at 31%.
   With 131K+ threads (deeper tree): each SM gets 32+ warps/SM = 67% occupancy. This requires
   circular heap allocation AND a global barrier after the split phase to prevent cross-warp
   data corruption. Current implementation only supports split_depth == tree_depth.

2. **Reduce heap words per interaction** — currently ~14 words allocated per @spin step.
   Fusing adjacent interactions (e.g., REF→ALO→LAM→APP-LAM as one unit) would eliminate
   intermediate heap cells. This is conceptually close to AOT but could be done as
   pattern-specific interpreter fast-paths.

3. **Prefetching APP arg** — when entering APP, prefetch `HEAP[loc+1]` (the arg needed later
   in apply). Currently the arg load and fun load are at different times. Prefetching would
   pipeline them. However, they're usually in the same 128B cache line (adjacent 8B words),
   so the arg is already in L1 from the fun load. Marginal expected benefit.

4. **Per-thread arena allocation** — each thread bumps a register-local pointer instead of
   going through warp-cooperative `heap_alloc_coop`. Eliminates `__activemask` + `__shfl_sync`
   overhead per allocation (~50 cycles/call). But allocations from different threads wouldn't
   coalesce (different address ranges), increasing cache line count by 32x for writes. Net
   effect is negative at scale due to bandwidth.

## Architecture Notes

- **Heap**: flat `u64[]` global memory, per-warp bump alloc (warp-cooperative, contiguous
  within warp). `heap_alloc` inline fast-path for count==1, noinline `heap_alloc_coop` for
  warp-cooperative path with `__shfl_sync` rank assignment + `atomicAdd` fallback.
  With `CIRCULAR_HEAP`: wraps to `S_WARP_BASE` when `S_WARP_HEAP` exceeds `S_WARP_END`.
- **Book**: `u64[2^24]` read-only, `__ldg` for lookups.
- **Stacks**: global memory, depth-major interleaved: `stacks[depth * n_threads + tid]`.
  `d_wnf_stride` = n_threads (1 for eval_kernel, n_threads for par_eval_kernel).
- **Hot state**: shared memory `s_hot[536]`:
  - `[0..255]` ITRS per-thread, `[256..511]` MAX_DEPTH per-thread.
  - `[512..519]` WARP_HEAP per-warp, `[520..527]` WARP_END per-warp.
  - `[528..535]` WARP_BASE per-warp (circular heap region start).
- **Split model**: static path mapping by thread ID bits. Owner reduces + `__threadfence`,
  others `spin_until_whnf` (volatile reads + `__nanosleep(100)` after 64 polls).
  `__syncwarp` after split for reconvergence.
- **Block size**: 1 for ≤32 threads (no intra-warp divergence), 128 for >32.
- **Memory budget**: ~21 GB heap, 128 MB book, per-thread 1024-entry WNF stacks (interleaved),
  S_HOT_BYTES = 4288 bytes shared memory per block.
