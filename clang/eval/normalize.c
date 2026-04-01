// Parallel normalization (SNF) traversal using work-stealing.
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>

#ifndef EVAL_NORMALIZE_WS_CAP_POW2
#define EVAL_NORMALIZE_WS_CAP_POW2 21
#endif

typedef struct __attribute__((aligned(64))) {
  WsDeque dq;
} EvalNormalizeWorker;

typedef struct {
  EvalNormalizeWorker   W[MAX_THREADS];
  Uset        seen;
  u32         n;
  _Alignas(CACHE_L1) CachePaddedAtomic pending;
} EvalNormalizeCtx;

typedef struct {
  EvalNormalizeCtx *ctx;
  u32 tid;
} EvalNormalizeArg;


static inline void eval_normalize_go(EvalNormalizeCtx *ctx, EvalNormalizeWorker *worker, u64 loc);

static inline void eval_normalize_enqueue(EvalNormalizeCtx *ctx, EvalNormalizeWorker *worker, u64 loc) {
  if (!wsq_push(&worker->dq, (u64)loc)) {
    eval_normalize_go(ctx, worker, loc);
  }
}

static inline void eval_normalize_go(EvalNormalizeCtx *ctx, EvalNormalizeWorker *worker, u64 loc) {
  for (;;) {
    if (__builtin_expect(GC_NEEDED, 0)) return;
    if (loc == 0 || !uset_add(&ctx->seen, loc)) {
      return;
    }
    Term term = __builtin_expect(STEPS_ENABLE, 0) ? wnf_steps_at(loc) : wnf_at(loc);
    if (__builtin_expect(GC_NEEDED, 0)) return;
    u64 tloc = term_val(term);
    u8  tag  = term_tag(term);
    if (tag == DP0 || tag == DP1) {
      loc = tloc;
      continue;
    }
    u32 ari = term_arity(term);
    if (ari == 0) {
      return;
    }
    for (u32 i = ari; i > 1; i--) {
      eval_normalize_enqueue(ctx, worker, tloc + (i - 1));
    }
    loc = tloc;
  }
}


static void *eval_normalize_worker(void *arg) {
  EvalNormalizeArg *A = (EvalNormalizeArg *)arg;
  EvalNormalizeCtx *ctx = A->ctx;
  u32 me = A->tid;
  EvalNormalizeWorker *worker = &ctx->W[me];

  wnf_set_tid(me);

  u32 n = ctx->n;
  u32 r = 0x9E3779B9u ^ me;
  bool active = true;

  for (;;) {
    if (__builtin_expect(GC_NEEDED, 0)) break;
    u64 task;
    if (atomic_load_explicit(&ctx->pending.v, memory_order_acquire) == 0) {
      break;
    }
    if (wsq_pop(&worker->dq, &task)) {
      u64 loc = task;
      eval_normalize_go(ctx, worker, loc);
      continue;
    }
    
    u32 stolen = false;
    u32 start  = (me + 1 + (r & 7)) % n;
    r ^= r << 13;
    r ^= r >> 17;
    r ^= r << 5;
    
    for (u32 k = 0; k < n - 1; k++) {
      u32 vic = (start + k) % n;
      if (vic == me) {
        continue;
      }
      if (wsq_can_steal(&ctx->W[vic].dq)) {
        if (!active) {
          atomic_fetch_add_explicit(&ctx->pending.v, 1, memory_order_release);
          active = true;
        }
        if (wsq_steal(&ctx->W[vic].dq, &task)) {
          stolen = true;
          u64 loc = task;
          eval_normalize_go(ctx, worker, loc);
          break;
        }
      }
    }
      
    if (stolen) {
      continue;
    }
    
    if (active) {
      atomic_fetch_sub_explicit(&ctx->pending.v, 1, memory_order_release);
      active = false;
    }

    sched_yield();
  }

  return NULL;
}

fn void uset_clear_range(Uset *set, u64 lo, u64 hi) {
  u64 w_lo = lo >> 6;
  u64 w_hi = (hi + 63) >> 6;
  if (w_hi > set->word_count) w_hi = set->word_count;
  for (u64 i = w_lo; i < w_hi; i++) {
    atomic_store_explicit(&set->words[i], 0, memory_order_relaxed);
  }
}

fn void wsq_reset(WsDeque *q) {
  atomic_store_explicit(&q->top.v, 0, memory_order_relaxed);
  atomic_store_explicit(&q->bot.v, 0, memory_order_relaxed);
}

fn Term eval_normalize(Term term) {
  wnf_set_tid(0);

  u64 root_loc = heap_alloc(1);
  heap_set(root_loc, term);

  if (STEPS_ENABLE) {
    STEPS_ROOT_LOC = root_loc;
    if (!SILENT) {
      print_term(heap_read(root_loc));
      printf("\n");
    }
  }

  EvalNormalizeCtx ctx;

  u32 n = thread_get_count();
  ctx.n = n;
  for (u32 i = 0; i < n; i++) {
    if (!wsq_init(&ctx.W[i].dq, EVAL_NORMALIZE_WS_CAP_POW2)) {
      fprintf(stderr, "eval_normalize: queue allocation failed\n");
      exit(1);
    }
  }
  uset_init(&ctx.seen);

  u64 gc_itrs_accum[MAX_THREADS] = {0};

  for (;;) {
    GC_NEEDED = 0;
    atomic_store_explicit(&ctx.pending.v, n, memory_order_relaxed);
    eval_normalize_enqueue(&ctx, &ctx.W[0], root_loc);

    pthread_t tids[MAX_THREADS];
    EvalNormalizeArg args[MAX_THREADS];
    for (u32 i = 1; i < n; i++) {
      args[i].ctx = &ctx;
      args[i].tid = i;
      pthread_create(&tids[i], NULL, eval_normalize_worker, &args[i]);
    }

    EvalNormalizeArg arg0 = { .ctx = &ctx, .tid = 0 };
    eval_normalize_worker(&arg0);

    for (u32 i = 1; i < n; i++) {
      pthread_join(tids[i], NULL);
    }

    if (!GC_NEEDED) break;

    for (u32 i = 0; i < n; i++) {
      gc_itrs_accum[i] += WNF_ITRS_BANKS[i].itrs;
    }
    root_loc = gc_collect(root_loc);
    uset_clear_range(&ctx.seen, GC_FROM_START, GC_FROM_START + GC_SPACE_SZ);
    uset_clear_range(&ctx.seen, GC_TO_START, GC_TO_START + GC_SPACE_SZ);
    for (u32 i = 0; i < n; i++) {
      wsq_reset(&ctx.W[i].dq);
    }
  }

  for (u32 i = 0; i < n; i++) {
    WNF_ITRS_BANKS[i].itrs += gc_itrs_accum[i];
  }

  for (u32 i = 0; i < n; i++) {
    wsq_free(&ctx.W[i].dq);
  }
  uset_free(&ctx.seen);

  if (STEPS_ENABLE) {
    STEPS_ROOT_LOC = 0;
  }

  return heap_read(root_loc);
}
