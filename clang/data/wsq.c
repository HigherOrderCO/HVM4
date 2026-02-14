// data/wsq.c - Resizable Chase-Lev work-stealing deque for u64 tasks.
//
// Context
// - Used by parallel evaluators to distribute heap locations across workers.
// - Single-owner pushes and pops from the bottom; other threads steal from the top.
//
// Design
// - Circular array (WsqArray) of power-of-two capacity storing u64 tasks.
// - Owner-initiated 2x growth when full: allocate new array, copy live elements,
//   atomically publish new array pointer (release). Old arrays are deferred-freed.
// - Thieves load array pointer with acquire after loading top — they see either
//   old or new array. Old array data is valid (never written after growth).
//   CAS on top prevents double-consumption regardless of which array was read.
// - Atomic top/bottom indices are cache-line padded to limit false sharing.
//
// Notes
// - Not multi-producer: only the owner thread may push/pop/grow.
// - wsq_push grows the deque if full; returns 0 only on OOM (fatal).
// - Counters are monotonic; wrap-around is not guarded (practically unreachable).

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

// Backing array for the deque (swapped atomically on growth).
typedef struct {
  u64    *buf;
  size_t  mask;  // cap - 1
} WsqArray;

// Maximum number of old arrays kept alive until wsq_free.
#define WSQ_PREV_MAX 16

// Work-stealing deque state (single owner, multi-stealer).
typedef struct __attribute__((aligned(CACHE_L1))) {
  _Alignas(CACHE_L1) CachePaddedAtomic top;
  _Alignas(CACHE_L1) CachePaddedAtomic bot;
  _Alignas(CACHE_L1) _Atomic(WsqArray *) arr;
  WsqArray *prev[WSQ_PREV_MAX];
  u32 prev_count;
} WsDeque;

// Allocate aligned memory for the ring buffer.
static inline void *wsq_aligned_alloc(size_t alignment, size_t nbytes) {
  void *ptr = NULL;
  size_t size = ((nbytes + alignment - 1) / alignment) * alignment;
  int err = posix_memalign(&ptr, alignment, size);
  if (err) {
    return NULL;
  }
  return ptr;
}

// Allocate a new WsqArray with the given capacity.
static inline WsqArray *wsq_array_new(size_t cap) {
  WsqArray *a = (WsqArray *)malloc(sizeof(WsqArray));
  if (!a) return NULL;
  a->buf = (u64 *)wsq_aligned_alloc(CACHE_L1, cap * sizeof(u64));
  if (!a->buf) { free(a); return NULL; }
  a->mask = cap - 1;
  return a;
}

// Free a WsqArray and its buffer.
static inline void wsq_array_free(WsqArray *a) {
  if (a) {
    free(a->buf);
    free(a);
  }
}

// Owner-only: grow the deque to 2x capacity. Returns new array.
// Copies live elements [top, bot) from old to new.
// Old array is stashed in prev[] for deferred free.
static inline WsqArray *wsq_grow(WsDeque *q, WsqArray *old, u64 bot, u64 top) {
  size_t new_cap = (old->mask + 1) * 2;
  WsqArray *a = wsq_array_new(new_cap);
  if (!a) {
    fprintf(stderr, "wsq_grow: allocation failed (new_cap=%zu)\n", new_cap);
    exit(1);
  }
  // Copy live elements from old to new array.
  for (u64 i = top; i < bot; i++) {
    a->buf[i & a->mask] = old->buf[i & old->mask];
  }
  // Stash old array for deferred free.
  if (q->prev_count < WSQ_PREV_MAX) {
    q->prev[q->prev_count++] = old;
  }
  // Publish new array (thieves will see it after acquire on arr).
  atomic_store_explicit(&q->arr, a, memory_order_release);
  return a;
}

// Initialize a deque with 2^capacity_pow2 slots.
static inline int wsq_init(WsDeque *q, u32 capacity_pow2) {
  size_t cap = (size_t)1 << capacity_pow2;
  WsqArray *a = wsq_array_new(cap);
  if (!a) return 0;
  atomic_store_explicit(&q->arr, a, memory_order_relaxed);
  atomic_store_explicit(&q->top.v, 0, memory_order_relaxed);
  atomic_store_explicit(&q->bot.v, 0, memory_order_relaxed);
  q->prev_count = 0;
  return 1;
}

// Release the deque: free current array and all stashed old arrays.
static inline void wsq_free(WsDeque *q) {
  if (!q) return;
  WsqArray *a = atomic_load_explicit(&q->arr, memory_order_relaxed);
  wsq_array_free(a);
  for (u32 i = 0; i < q->prev_count; i++) {
    wsq_array_free(q->prev[i]);
  }
  q->prev_count = 0;
  atomic_store_explicit(&q->arr, NULL, memory_order_relaxed);
}

// Owner push to the bottom; grows if full. Returns 1 always (exits on OOM).
static inline int wsq_push(WsDeque *q, u64 x) {
  u64 b = atomic_load_explicit(&q->bot.v, memory_order_relaxed);
  u64 t = atomic_load_explicit(&q->top.v, memory_order_acquire);
  WsqArray *a = atomic_load_explicit(&q->arr, memory_order_relaxed);
  if (__builtin_expect(b - t > a->mask, 0)) {
    a = wsq_grow(q, a, b, t);
  }
  a->buf[b & a->mask] = x;
  atomic_store_explicit(&q->bot.v, b + 1, memory_order_release);
  return 1;
}

// Owner pop from the bottom; returns 1 on success, 0 if empty or lost race.
static inline int wsq_pop(WsDeque *q, u64 *out) {
  u64 b = atomic_load_explicit(&q->bot.v, memory_order_relaxed);
  if (b == 0) {
    return 0;
  }
  u64 b1 = b - 1;
  atomic_store_explicit(&q->bot.v, b1, memory_order_release);
  atomic_thread_fence(memory_order_seq_cst);

  u64 t = atomic_load_explicit(&q->top.v, memory_order_acquire);
  if (t <= b1) {
    WsqArray *a = atomic_load_explicit(&q->arr, memory_order_relaxed);
    u64 x = a->buf[b1 & a->mask];
    if (t == b1) {
      u64 expected = t;
      bool ok = atomic_compare_exchange_strong_explicit(
        &q->top.v,
        &expected,
        t + 1,
        memory_order_acq_rel,
        memory_order_acquire
      );
      if (!ok) {
        atomic_store_explicit(&q->bot.v, t + 1, memory_order_release);
        return 0;
      }
      atomic_store_explicit(&q->bot.v, t + 1, memory_order_release);
    }
    *out = x;
    return 1;
  } else {
    atomic_store_explicit(&q->bot.v, t, memory_order_release);
    return 0;
  }
}

// Thief steal from the top; returns 1 on success, 0 if empty or lost race.
static inline int wsq_steal(WsDeque *q, u64 *out) {
  u64 t = atomic_load_explicit(&q->top.v, memory_order_acquire);
  u64 b = atomic_load_explicit(&q->bot.v, memory_order_acquire);
  if (t >= b) {
    return 0;
  }
  WsqArray *a = atomic_load_explicit(&q->arr, memory_order_acquire);
  u64 x = a->buf[t & a->mask];
  u64 expected = t;
  bool ok = atomic_compare_exchange_strong_explicit(
    &q->top.v,
    &expected,
    t + 1,
    memory_order_acq_rel,
    memory_order_acquire
  );
  if (ok) {
    *out = x;
    return 1;
  }
  return 0;
}

// Check if there are stealable items (non-binding).
static inline bool wsq_can_steal(WsDeque *q) {
  u64 t = atomic_load_explicit(&q->top.v, memory_order_acquire);
  u64 b = atomic_load_explicit(&q->bot.v, memory_order_acquire);
  return t < b;
}

// Read current capacity (for external checks like deadlock detection).
static inline size_t wsq_capacity(WsDeque *q) {
  WsqArray *a = atomic_load_explicit(&q->arr, memory_order_relaxed);
  return a ? a->mask + 1 : 0;
}
