// data/wsq.c - Chase-Lev work-stealing deque for u64 tasks.
//
// Context
// - Used by parallel evaluators to distribute heap locations across workers.
// - Single-owner pushes and pops from the bottom; other threads steal from the top.
//
// Design
// - Ring buffer of fixed capacity (power of two) storing u64 tasks.
// - Atomic top/bottom indices are cache-line padded to limit false sharing.
// - Owner operations are wait-free except for full/empty checks.
// - Steals are lock-free and may fail under contention.
//
// Notes
// - Not multi-producer: only the owner thread may push/pop.
// - Capacity is fixed after init; wsq_push returns 0 when full.
// - Counters are monotonic; wrap-around is not guarded (practically unreachable).

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#if HVM_WINDOWS
#include <malloc.h>
#endif

#if defined(__clang__) || defined(__GNUC__)
#define WSQ_PREFETCH(ptr, rw, locality) __builtin_prefetch((ptr), (rw), (locality))
#else
#define WSQ_PREFETCH(ptr, rw, locality) ((void)0)
#endif

// Work-stealing deque state (single owner, multi-stealer).
typedef struct __attribute__((aligned(CACHE_L1))) {
  _Alignas(CACHE_L1) CachePaddedAtomic top;
  _Alignas(CACHE_L1) CachePaddedAtomic bot;
  _Alignas(CACHE_L1) u64 *buf;
  size_t mask;
  size_t cap;
} WsDeque;

// Allocate aligned memory for the ring buffer.
static inline void *wsq_aligned_alloc(size_t alignment, size_t nbytes) {
#if HVM_WINDOWS
  return _aligned_malloc(nbytes, alignment);
#else
  void *ptr = NULL;
  size_t size = ((nbytes + alignment - 1) / alignment) * alignment;
  int err = posix_memalign(&ptr, alignment, size);
  if (err) {
    return NULL;
  }
  return ptr;
#endif
}

// Initialize a deque with 2^capacity_pow2 slots.
static inline int wsq_init(WsDeque *q, u32 capacity_pow2) {
  size_t cap = (size_t)1 << capacity_pow2;
  q->buf = (u64 *)wsq_aligned_alloc(CACHE_L1, cap * sizeof(u64));
  if (!q->buf) {
    return 0;
  }
  q->cap  = cap;
  q->mask = cap - 1;
  atomic_store_explicit(&q->top.v, 0, memory_order_relaxed);
  atomic_store_explicit(&q->bot.v, 0, memory_order_relaxed);
  return 1;
}

// Release the deque buffer.
static inline void wsq_free(WsDeque *q) {
  if (q && q->buf) {
#if HVM_WINDOWS
    _aligned_free(q->buf);
#else
    free(q->buf);
#endif
    q->buf = NULL;
  }
}

// Owner push to the bottom; returns 1 on success, 0 if full.
static inline int wsq_push(WsDeque *q, u64 x) {
  u64 b = atomic_load_explicit(&q->bot.v, memory_order_relaxed);
  u64 t = atomic_load_explicit(&q->top.v, memory_order_acquire);
  if (b - t >= q->cap) {
    return 0;
  }
  WSQ_PREFETCH(&q->buf[b & q->mask], 1, 1);
  q->buf[b & q->mask] = x;
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
  WSQ_PREFETCH(&q->buf[b1 & q->mask], 0, 1);
  atomic_store_explicit(&q->bot.v, b1, memory_order_release);
  atomic_thread_fence(memory_order_seq_cst);

  u64 t = atomic_load_explicit(&q->top.v, memory_order_acquire);
  if (t <= b1) {
    u64 x = q->buf[b1 & q->mask];
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
  WSQ_PREFETCH(&q->buf[t & q->mask], 0, 1);
  u64 x = q->buf[t & q->mask];
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


static inline bool wsq_can_steal(WsDeque *q) {
  u64 t = atomic_load_explicit(&q->top.v, memory_order_acquire);
  u64 b = atomic_load_explicit(&q->bot.v, memory_order_acquire);
  return t < b;
}