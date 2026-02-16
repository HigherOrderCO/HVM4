// Per-thread, size-segregated free lists for heap memory recycling.
// Each freed block stores a "next" pointer in HEAP[loc].
// Free list heads are per-thread to avoid contention.

#define FREE_LIST_MAX_SIZE 16
#define FREE_STRIDE 16  // 16 u64s = 128 bytes per thread (cache-line aligned)

static u64 FREE_HEADS[MAX_THREADS * FREE_STRIDE] __attribute__((aligned(128))) = {0};

#define FREE_HEAD_AT(tid, sz) FREE_HEADS[(u64)(tid) * FREE_STRIDE + (sz) - 1]

fn void heap_free(u64 loc, u64 size) {
  // Disabled in multi-threaded mode: cross-thread DUP interactions can free
  // blocks from another thread's heap slice, causing race conditions.
  // In single-threaded mode, the free list is safe and improves memory reuse.
  if (__builtin_expect(THREAD_COUNT > 1, 0)) return;
  if (__builtin_expect(size == 0 || size > FREE_LIST_MAX_SIZE, 0)) return;
  u32 tid = WNF_TID;
  HEAP[loc] = FREE_HEAD_AT(tid, size);
  FREE_HEAD_AT(tid, size) = loc;
}

fn void heap_free_reset(void) {
  memset(FREE_HEADS, 0, sizeof(FREE_HEADS));
}
