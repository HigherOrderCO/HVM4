# HVM4 CUDA Port - Context for Continuation

## What Exists

### Files
- `cuda/hvm.cu` — GPU evaluator (~1475 lines). Shares interaction rules from `clang/` via
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
```

### RTX Machine
- `ssh rtx` (may need `ssh-add ~/.ssh/id_rsa`)
- RTX 4090, 24 GB VRAM, SM 8.9 (Ada Lovelace), CUDA 12.4, Ryzen 7900X
- 128 SMs, 1 TB/s DRAM, 72 MB L2 cache, 128 KB L1 per SM, 65536 regs per SM
- Rsync: `rsync -azq /Users/v/t/dev/hvm/ rtx:~/hvm/ --exclude='.git'`

## Current Performance

| Threads | Old MIPS | New MIPS   | Change   |
|---------|----------|------------|----------|
| 1       | 0.77     | 0.85       | +10%     |
| 2       | 1.53     | 1.70       | +11%     |
| 1,024   | 335      | **681**    | +103%    |
| 4,096   | 1,313    | **2,750**  | +110%    |
| 16,384  | 3,847    | **10,674** | +177%    |
| 32,768  | —        | **18,634** | —        |
| 65,536  | 3,971    | **22,713** | **+472%**|

CPU baselines: Ryzen 77 MIPS/thread, M4 Max 184 MIPS/thread.

**22.7 GIPS peak at 65K threads (5.7x over prior 4 GIPS).** Per-thread scaling efficiency
at 65K: 41% (was 7.9%). Theoretical max: 0.85 × 65536 = 55.7 GIPS. Sweet spot is 32K
threads (18.6 GIPS); 65K still scales due to latency hiding despite DRAM saturation.

### Register / Occupancy Profile
- `par_eval_kernel`: 58 registers, 384 bytes stack, 0 spills → 8 blocks/SM (1024 threads,
  32 warps) at block_size=128.
- `eval_kernel`: 47 registers, 128 bytes stack, 0 spills.
- `heap_alloc_coop`: noinline, ~20 registers. Keeps warp shuffle/vote logic out of wnf().

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
Total: 528 u64 = 4224 bytes per block.
```
- `d_itrs` macro = `s_hot[S_ITRS + threadIdx.x]`.
- `c_arity[48]`: device-const arity lookup table, replaces 40-case switch.

### Heap Allocator (lines 209–239)
Two-tier design:
1. **`heap_alloc(size)`** (forceinline): calls `__activemask()`, checks `__popc(mask)`.
   If count==1 (single active thread): simple shared-memory bump. Otherwise → coop path.
2. **`heap_alloc_coop(size)`** (noinline): warp-cooperative allocation.
   - Gets fresh `__activemask()` inside (critical for SM89 independent thread scheduling —
     passing a stale mask from the caller causes illegal memory access).
   - Uniform-size fast path: leader bumps per-warp pointer by `count * size`, broadcasts
     base via `__shfl_sync`, each thread gets `base + rank * size`. All warp allocations
     are contiguous → writes coalesce into ~4 cache lines instead of 32.
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
- **aot_try_call stub**: returns 0 (no AOT yet). Signature: `(u32 id, u32 *s_pos, u32 base, Term *out)`.

### wnf_at + is_whnf_tag (lines 1094–1104)
- `is_whnf_tag(tag)`: `(WHNF_MASK >> tag) & 1`. Single-instruction check.
- `wnf_at(loc)`: reads cell, short-circuits if WHNF, else calls wnf() and writes back.

### eval_kernel (lines 1110–1130)
Single-thread WNF evaluation. Sets `d_wnf_stride = 1`. Initializes one warp heap region
covering the full heap. Returns result term + itrs + max_depth + heap_next.

### par_eval_kernel (lines 1164–1210)
Parallel SNF evaluation via static binary-tree split:
1. **Heap init**: per-warp regions. `tpr` (threads per region) = 32 if block_size≥32, else 1.
   `n_regions = n_threads / tpr`. Each region gets `heap_size / n_regions` words.
   `__syncthreads()` after init (needed because different warps' lane-0 threads init their
   warp's pointers and all threads must see them).
2. **Split loop**: for depth 0..split_depth-1, owner thread (lowest matching bits) calls
   `wnf_at(loc)` + `__threadfence()`, others `spin_until_whnf(loc)` with nanosleep after
   64 polls. Each thread follows its bit path down the tree.
3. **`__syncwarp()`** after split for reconvergence.
4. **`seq_snf(loc)`**: iterative SNF with 32-entry work stack. Calls wnf_at on each subterm.
5. Results: `atomicAdd` for total itrs, `atomicMax` for max depth.

### Host Code (lines 1220–1475)
- `run_eval()`: single-thread mode. Allocates 6 GB heap, 128 MB book, 1 GB WNF stack.
- `run_par_eval()`: parallel mode. Block size: 1 for ≤32 threads, 128 for >32.
  Stack: 1024 entries/thread (interleaved). Heap: fills available VRAM minus overhead.
- `-p DEPTH` flag: `n_threads = 1 << depth`, `split_depth = depth`.

## Why Per-Thread Rate Drops at Scale

### The @spin Hot Path (per step, ~6 interactions, ~21 WNF dispatches)
1. REF(@spin) → BOOK lookup (__ldg) → ALO
2. ALO → wnf_alo_lam: heap_alloc(2), heap_alloc(1), 3 heap_set → LAM
3. APP + LAM → wnf_app_lam: heap_read, heap_subst_var → body (ALO wrapping MAT)
4. ALO → wnf_alo_nod: heap_alloc(2), 2 heap_set → MAT{ALO, ALO}
5. MAT + NUM → wnf_app_mat_num: 2 heap_read, 2 heap_set (reuse loc) → APP(else, num)
6. OP2 + NUM + NUM → wnf_op2_num_num: pure ALU → NUM

~20 heap accesses per step, ~8 heap_alloc calls per step (including term_new_alo calls).

### Memory Traffic Analysis (65K threads, bench_tree)
- Per step per warp: ~20 heap ops × 128 bytes/cache-line × (coalesced or random)
- Total steps/s: 65K × (22.7M itrs/s / 65K) / 6 ≈ 575M steps/s
- DRAM bandwidth wall: 1 TB/s. With ~60-70% L2 hit rate, DRAM demand ≈ 0.8-1 TB/s.
- Warp-cooperative allocator coalesces freshly allocated cells (biggest traffic source).
  Remaining random traffic: BOOK body reads, substitution lookups to older allocations.

### Why 32K ≈ 65K Throughput
- 58 regs → 1024 threads/SM max → 128 SMs × 1024 = 131K slots. Both 32K and 65K fit.
- At 32K: each thread does 2x work (403M/32K vs 403M/65K interactions). More L2-resident
  working set per thread → higher per-thread rate → same total throughput.
- At 65K: per-thread rate drops (more L2 pressure), but more parallelism compensates.
  Net result: ~same total MIPS.

## What Was Tried and Didn't Help (or Marginal)

- **`__launch_bounds__(128, 4)`** — forced 128 regs, 4 blocks/SM. Helped 65K (+10%) but hurt
  32K (-7%) due to 156-byte spills. Removed; noinline heap_alloc_coop achieves 58 regs
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

## What Would Further Improve Performance

1. **AOT compilation via `aot_try_call()`** — eliminates the interpreter loop entirely.
   Each @spin step's ~21 switch dispatches collapse to direct function calls. The hook exists
   in wnf(). When the unified AOT compiler emits `fn`/`heap_*`/`ITRS_INC`-based functions,
   they compile on both CPU and GPU unchanged. Expected 3-5x per-thread improvement.

2. **Vectorized heap writes** — pair adjacent `heap_set` calls into 128-bit stores
   (`ulonglong2`) where alignment permits (e.g., ALO-LAM's two writes to bind_loc).
   Halves write transactions for 2-cell allocations.

3. **Tighter heap layout** — dynamically size per-warp regions based on actual allocation
   needs (currently each warp gets heap_size/n_warps regardless of workload). Would improve
   L2 utilization for irregular workloads where some warps allocate much more than others.

4. **Compact heap via smaller per-warp chunks** — cap per-warp heap at estimated need
   (e.g., 8 MB) rather than equal-dividing the full 20 GB. Threads in neighboring warps
   would be closer in address space → better L2 set utilization. Requires knowing per-thread
   allocation bound at launch time.

## Architecture Notes

- **Heap**: flat `u64[]` global memory, per-warp bump alloc (warp-cooperative, contiguous
  within warp). `heap_alloc` inline fast-path for count==1, noinline `heap_alloc_coop` for
  warp-cooperative path with `__shfl_sync` rank assignment + `atomicAdd` fallback.
- **Book**: `u64[2^24]` read-only, `__ldg` for lookups.
- **Stacks**: global memory, depth-major interleaved: `stacks[depth * n_threads + tid]`.
  `d_wnf_stride` = n_threads (1 for eval_kernel, n_threads for par_eval_kernel).
- **Hot state**: shared memory `s_hot[528]`:
  - `[0..255]` ITRS per-thread, `[256..511]` MAX_DEPTH per-thread.
  - `[512..519]` WARP_HEAP per-warp, `[520..527]` WARP_END per-warp.
- **Split model**: static path mapping by thread ID bits. Owner reduces + `__threadfence`,
  others `spin_until_whnf` (volatile reads + `__nanosleep(100)` after 64 polls).
  `__syncwarp` after split for reconvergence.
- **Block size**: 1 for ≤32 threads (no intra-warp divergence), 128 for >32.
- **Memory budget**: ~21 GB heap, 128 MB book, per-thread 1024-entry WNF stacks (interleaved),
  S_HOT_BYTES = 4224 bytes shared memory per block.
