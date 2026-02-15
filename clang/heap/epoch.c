// heap/epoch.c — Ring-backed epoch nursery for HVM4
//
// Splits HEAP into a stable region (for compacted data) and per-thread nursery
// slices (for ephemeral evaluation intermediates). At epoch boundary, nursery
// pointers reset in O(1), bulk-reclaiming all evaluation garbage.
//
// IC's cycle-freedom guarantees: after normalization to SNF, all nursery data
// except the result tree is dead. compact_deep_copy moves the live result to
// stable, making nursery safe to reclaim without tracing.
//
// heap_alloc() is UNCHANGED — it bumps in the thread's slice (which epoch_init
// points to the nursery region). Zero allocation overhead vs. current allocator.

static int EPOCH_ENABLED = 0;
static u32 EPOCH_DEPTH   = 0;  // Nesting depth for nested @compact calls

// Stable region: [1, STABLE_CAP)
static u64 STABLE_CAP  = 0;
static u64 STABLE_NEXT = 0;

// Per-thread nursery start positions (for reset)
// Uses same HEAP_STRIDE padding as HEAP_NEXT/HEAP_END for cache alignment
static u64 NURSERY_START[MAX_THREADS * HEAP_STRIDE] __attribute__((aligned(256))) = {0};
#define NURSERY_START_AT(t) NURSERY_START[(u64)(t) * HEAP_STRIDE]

// Stats
typedef struct {
  u64 epochs;
  u64 nursery_words_reclaimed;
  u64 stable_words_allocated;
  u64 peak_nursery_used;
} EpochStats;

static EpochStats EPOCH_STATS = {0};

// ============================================================
// Initialization
// ============================================================

// Initialize epoch mode. Replaces heap_init_slices().
// stable_fraction: denominator for stable size (4 = 1/4 of HEAP for stable).
// Requires stable_fraction >= 2 (at least half the heap for nursery).
fn void epoch_init(u32 stable_fraction) {
  u32 threads = thread_get_count();
  u64 words   = HEAP_CAP;

  // Stable region: first 1/stable_fraction of HEAP
  if (stable_fraction < 2) stable_fraction = 4;
  STABLE_CAP  = words / stable_fraction;
  if (STABLE_CAP < 4096) STABLE_CAP = 4096;
  STABLE_NEXT = 1;  // Skip location 0 (null sentinel)

  // Nursery region: rest of HEAP, split equally per thread
  u64 nursery_total      = words - STABLE_CAP;
  u64 nursery_per_thread = nursery_total / threads;

  for (u32 t = 0; t < threads; t++) {
    u64 start = STABLE_CAP + (u64)t * nursery_per_thread;
    u64 end   = (t == threads - 1) ? words : start + nursery_per_thread;
    NURSERY_START_AT(t) = start;
    HEAP_NEXT_AT(t)     = start;
    HEAP_END_AT(t)      = end;
  }

  // Clear unused thread slots
  for (u32 t = threads; t < MAX_THREADS; t++) {
    NURSERY_START_AT(t) = 0;
    HEAP_NEXT_AT(t)     = 0;
    HEAP_END_AT(t)      = 0;
  }

  EPOCH_ENABLED = 1;
  EPOCH_DEPTH   = 0;
  memset(&EPOCH_STATS, 0, sizeof(EPOCH_STATS));
}

// ============================================================
// Stable allocation
// ============================================================

// Allocate from the stable region. Single-threaded: only called from
// compact_deep_copy on thread 0, after eval_normalize has joined all threads.
fn u64 heap_alloc_stable(u64 size) {
  u64 at   = STABLE_NEXT;
  u64 next = at + size;
  if (__builtin_expect(next <= STABLE_CAP && next >= at, 1)) {
    STABLE_NEXT = next;
    EPOCH_STATS.stable_words_allocated += size;
    return at;
  }
  fprintf(stderr,
    "Out of stable heap memory (used %llu / %llu words, need %llu)\n"
    "Hint: use --epoch=N with smaller N for more stable space\n",
    (unsigned long long)STABLE_NEXT,
    (unsigned long long)STABLE_CAP,
    (unsigned long long)size);
  exit(1);
}

// ============================================================
// Epoch lifecycle
// ============================================================

// Begin a new epoch. Supports nesting: only the outermost begin/reset pair
// actually resets the nursery. Inner @compact calls are no-ops for epoch lifecycle.
fn void epoch_begin(void) {
  EPOCH_DEPTH++;
}

// Reset all nursery regions. O(threads) pointer rewinds.
// MUST be called after compact has copied all live data to stable.
// After this call, ALL nursery locations contain stale data.
// Supports nesting: only the outermost reset actually reclaims memory.
fn void epoch_reset(void) {
  if (EPOCH_DEPTH > 1) {
    EPOCH_DEPTH--;
    return;  // Inner @compact — don't reset nursery
  }
  EPOCH_DEPTH = 0;

  u32 threads = thread_get_count();
  u64 total_used = 0;

  for (u32 t = 0; t < threads; t++) {
    u64 used = HEAP_NEXT_AT(t) - NURSERY_START_AT(t);
    total_used += used;
    HEAP_NEXT_AT(t) = NURSERY_START_AT(t);
  }

  // Clear free lists (may contain nursery locations from heap_free)
  heap_free_reset();

  EPOCH_STATS.nursery_words_reclaimed += total_used;
  if (total_used > EPOCH_STATS.peak_nursery_used) {
    EPOCH_STATS.peak_nursery_used = total_used;
  }
  EPOCH_STATS.epochs++;
}

// Debug variant: zero nursery memory after reset.
// WARNING: Writing 0 to nursery slots makes heap_read() spin-wait on those
// locations. Only use single-threaded when no other thread could be reading.
// O(nursery_used) — catches stale-pointer bugs but is slower than epoch_reset.
fn void epoch_reset_zero(void) {
  if (EPOCH_DEPTH > 1) {
    EPOCH_DEPTH--;
    return;
  }
  EPOCH_DEPTH = 0;

  u32 threads = thread_get_count();
  u64 total_used = 0;

  for (u32 t = 0; t < threads; t++) {
    u64 start = NURSERY_START_AT(t);
    u64 end   = HEAP_NEXT_AT(t);
    u64 used  = end - start;
    total_used += used;
    if (used > 0) {
      memset(&HEAP[start], 0, used * sizeof(Term));
    }
    HEAP_NEXT_AT(t) = start;
  }

  heap_free_reset();

  EPOCH_STATS.nursery_words_reclaimed += total_used;
  if (total_used > EPOCH_STATS.peak_nursery_used) {
    EPOCH_STATS.peak_nursery_used = total_used;
  }
  EPOCH_STATS.epochs++;
}

// ============================================================
// Queries
// ============================================================

fn u64 epoch_nursery_used(u32 tid) {
  return HEAP_NEXT_AT(tid) - NURSERY_START_AT(tid);
}

fn u64 epoch_nursery_used_total(void) {
  u32 threads = thread_get_count();
  u64 total = 0;
  for (u32 t = 0; t < threads; t++) {
    total += HEAP_NEXT_AT(t) - NURSERY_START_AT(t);
  }
  return total;
}

fn u64 epoch_stable_used(void) {
  return STABLE_NEXT - 1;  // -1 because we start at 1
}

fn int epoch_is_stable(u32 loc) {
  return loc > 0 && (u64)loc < STABLE_CAP;
}

// Reset stable allocation pointer. Used by benchmarks to re-run stable alloc tests.
fn void epoch_reset_stable(void) {
  STABLE_NEXT = 1;
}

// ============================================================
// Stats
// ============================================================

fn void epoch_print_stats(void) {
  if (!EPOCH_ENABLED) return;
  fprintf(stderr, "[epoch] mode: enabled (stable=1/%llu of HEAP)\n",
    (unsigned long long)(HEAP_CAP / STABLE_CAP));
  fprintf(stderr, "[epoch] epochs completed: %llu\n",
    (unsigned long long)EPOCH_STATS.epochs);
  fprintf(stderr, "[epoch] stable: %llu / %llu words (%.1f%%)\n",
    (unsigned long long)epoch_stable_used(),
    (unsigned long long)STABLE_CAP,
    100.0 * (double)epoch_stable_used() / (double)STABLE_CAP);
  fprintf(stderr, "[epoch] nursery reclaimed: %llu words total\n",
    (unsigned long long)EPOCH_STATS.nursery_words_reclaimed);
  fprintf(stderr, "[epoch] peak nursery per epoch: %llu words\n",
    (unsigned long long)EPOCH_STATS.peak_nursery_used);
}

// ============================================================
// Benchmark
// ============================================================

fn int epoch_bench(void) {
  fprintf(stderr, "[epoch_bench] Starting allocation benchmark...\n");

  // Setup: use epoch mode with stable = 1/4
  epoch_init(4);

  u64 nursery_cap = HEAP_END_AT(0) - NURSERY_START_AT(0);
  fprintf(stderr, "[epoch_bench] Nursery capacity (thread 0): %llu words (%.1f MB)\n",
    (unsigned long long)nursery_cap,
    (double)(nursery_cap * sizeof(Term)) / (1024.0 * 1024.0));
  fprintf(stderr, "[epoch_bench] Stable capacity: %llu words (%.1f MB)\n",
    (unsigned long long)STABLE_CAP,
    (double)(STABLE_CAP * sizeof(Term)) / (1024.0 * 1024.0));

  struct timespec t0, t1;

  // Benchmark 1: Nursery allocation throughput (single-threaded)
  {
    u64 alloc_count = 0;
    u64 alloc_words = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    // Allocate until nursery is ~80% full
    u64 limit = nursery_cap * 8 / 10;
    while (epoch_nursery_used(0) < limit) {
      u64 loc = heap_alloc(2);  // Typical: 2-word nodes
      heap_set((u32)loc, 42);
      heap_set((u32)(loc + 1), 43);
      alloc_count++;
      alloc_words += 2;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "[epoch_bench] Nursery alloc: %llu allocs (%llu words) in %.4f s = %.1f M allocs/s\n",
      (unsigned long long)alloc_count,
      (unsigned long long)alloc_words,
      dt, alloc_count / dt / 1e6);
  }

  // Benchmark 2: Epoch reset latency
  {
    u64 nursery_used = epoch_nursery_used(0);
    clock_gettime(CLOCK_MONOTONIC, &t0);

    u32 reset_count = 10000;
    for (u32 i = 0; i < reset_count; i++) {
      epoch_reset();
      // Re-fill a small amount so reset has something to do
      heap_alloc(16);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "[epoch_bench] Epoch reset: %u resets in %.6f s = %.0f ns/reset\n",
      reset_count, dt, dt / reset_count * 1e9);
    fprintf(stderr, "[epoch_bench]   (first reset reclaimed %llu words)\n",
      (unsigned long long)nursery_used);
  }

  // Benchmark 3: Stable allocation throughput
  {
    u64 alloc_count = 0;
    u64 limit = STABLE_CAP * 8 / 10;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (epoch_stable_used() < limit) {
      u64 loc = heap_alloc_stable(2);
      heap_set((u32)loc, 42);
      heap_set((u32)(loc + 1), 43);
      alloc_count++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "[epoch_bench] Stable alloc: %llu allocs in %.4f s = %.1f M allocs/s\n",
      (unsigned long long)alloc_count, dt, alloc_count / dt / 1e6);
  }

  // Benchmark 4: Multi-epoch simulation
  {
    // Reset everything for clean measurement
    epoch_reset_stable();
    epoch_reset();

    u32 num_epochs = 100;
    u64 allocs_per_epoch = 100000;
    u64 live_per_epoch   = 1000;  // Words that "survive" to stable

    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (u32 e = 0; e < num_epochs; e++) {
      epoch_begin();

      // Simulate normalization: many nursery allocs
      for (u64 i = 0; i < allocs_per_epoch; i++) {
        u64 loc = heap_alloc(2);
        heap_set((u32)loc, (Term)(i + 1));
        heap_set((u32)(loc + 1), (Term)(i + 2));
      }

      // Simulate compact: promote small amount to stable
      for (u64 i = 0; i < live_per_epoch; i++) {
        u64 loc = heap_alloc_stable(1);
        heap_set((u32)loc, (Term)(e * 1000 + i));
      }

      // Reset nursery
      epoch_reset();
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "[epoch_bench] Multi-epoch: %u epochs in %.4f s = %.2f ms/epoch\n",
      num_epochs, dt, dt / num_epochs * 1e3);
    fprintf(stderr, "[epoch_bench]   Nursery allocs/epoch: %llu (reclaimed each time)\n",
      (unsigned long long)allocs_per_epoch);
    fprintf(stderr, "[epoch_bench]   Stable after %u epochs: %llu words\n",
      num_epochs, (unsigned long long)epoch_stable_used());
    fprintf(stderr, "[epoch_bench]   Without epochs would use: %llu words\n",
      (unsigned long long)(num_epochs * allocs_per_epoch * 2));
  }

  epoch_print_stats();
  fprintf(stderr, "[epoch_bench] All benchmarks passed\n");
  return 0;
}
