# HVM4 Epoch Nursery Allocator

## Problem

The current HVM4 allocator is a per-thread bump allocator that never reclaims memory.
For iterative algorithms (e.g., Bellman-Ford pathfinding), each round allocates O(T)
words for evaluation intermediates, leading to O(R × T) total heap consumption over R
rounds. The heap exhausts at HEAP_CAP and the program crashes.

The `@compact` primitive deep-copies live data to fresh heap positions but doesn't
reclaim the old space. So total heap grows as O(R × T + R × live_size).

## Key Insight: IC is Cycle-Free

Interaction Calculus (IC) guarantees cycle-freedom through:
- **Affine variables**: each used at most once
- **Consumption-based rewriting**: interacting nodes are consumed
- **Write-once substitution**: variable binding is final

This means: after normalization to SNF, all evaluation intermediates are dead. Only the
result tree is live. No reference cycles exist. This makes epoch-based reclamation
provably complete — no GC tracing needed.

## Architecture

```
HEAP[0 ................. STABLE_CAP ................. HEAP_CAP]
     [--- Stable Region ---][--- Nursery (per-thread slices) ---]
```

### Stable Region: `HEAP[1, STABLE_CAP)`
- Holds compacted/promoted data that survives across epochs
- Single bump allocator (`STABLE_NEXT`)
- Called only from `compact_deep_copy` (single-threaded, after normalization)
- Grows monotonically; for long-running computations, periodic stable compaction
  can reclaim dead stable data (future work)

### Nursery: `HEAP[STABLE_CAP, HEAP_CAP)` (per-thread slices)
- Each thread gets `(HEAP_CAP - STABLE_CAP) / num_threads` words
- Same bump allocation as current `heap_alloc()` — zero overhead change
- At epoch boundary: reset `HEAP_NEXT[t]` to nursery start (O(1) per thread)
- Free lists cleared on reset (they may contain nursery locations)

## Epoch Lifecycle

```
┌─────────────────────────────────────────┐
│  1. epoch_begin()                       │  Mark epoch start
│  2. Build term (allocates in nursery)   │  Same as current heap_alloc
│  3. eval_normalize(term)                │  Multi-threaded normalization
│  4. compact_deep_copy(result)           │  Copy live → stable region
│  5. epoch_reset()                       │  O(1) nursery reclaim
│  6. Use compacted result for next round │
│  7. → goto 1                            │
└─────────────────────────────────────────┘
```

## Performance Characteristics

| Operation      | Complexity | Notes                              |
|----------------|------------|------------------------------------|
| Nursery alloc  | O(1)       | Bump pointer, identical to current |
| Epoch reset    | O(threads) | Pointer rewind per thread          |
| Compact        | O(live)    | Deep-copy of surviving data        |
| Stable alloc   | O(1)       | Bump pointer                       |

**Zero per-object overhead**: No headers, no refcounts, no tags. The nursery bump
allocator is byte-for-byte identical to the current `heap_alloc`.

**Memory savings**: Over R rounds with tree size T and nursery allocs N per round:
- Without epoch: O(R × N) total heap (linear growth → OOM)
- With epoch: O(R × T) stable + O(N) nursery (constant nursery, growing stable)
- Since T << N typically, this is a massive improvement

## API

```c
// Initialize epoch mode (replaces heap_init_slices)
void epoch_init(u32 stable_fraction);  // e.g., 4 = 1/4 of HEAP for stable

// Epoch lifecycle
void epoch_begin(void);               // Start new epoch
void epoch_reset(void);               // O(1) nursery reclaim (fast, no zeroing)
void epoch_reset_zero(void);          // Debug: zero nursery after reset

// Stable allocation (for compact)
u64  heap_alloc_stable(u64 size);     // Bump in stable region

// Stats
u64  epoch_nursery_used(u32 tid);     // Words used by thread this epoch
u64  epoch_nursery_used_total(void);  // Total across all threads
u64  epoch_stable_used(void);         // Stable words used
void epoch_print_stats(void);         // Print summary to stderr
```

## Integration

- `heap_alloc()` is **unchanged** — it bumps in the thread's slice, which epoch_init
  points to the nursery region instead of the full heap
- `compact_deep_copy()` uses `heap_alloc_stable()` when `EPOCH_ENABLED`
- `eval_normalize()` is unchanged — multi-threaded normalization works as before
- `epoch_reset()` must be called **after** all threads have joined (which
  `eval_normalize` guarantees with its pthread_join barrier)

## CLI

```
./main <file.hvm4> --epoch        # Enable epoch mode (stable = 1/4 HEAP)
./main <file.hvm4> --epoch=8      # Stable = 1/8 HEAP (larger nursery)
./main <file.hvm4> --epoch-bench  # Run allocation microbenchmark
```

## Future Work

- **Stable compaction**: When stable fills up, deep-copy live stable data to compact it
- **Ref-counted stable**: IC cycle-freedom makes per-object ref-counting trivially
  correct for stable data; could enable fine-grained stable recycling
- **Ring-backed nursery overflow**: If nursery is exhausted mid-epoch, spill to a
  secondary ElasticRing-backed overflow buffer (currently: error + exit)
- **Concurrent compact**: Parallelize deep-copy across threads for large result trees
