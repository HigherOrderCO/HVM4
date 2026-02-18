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


static inline void eval_normalize_go(EvalNormalizeCtx *ctx, EvalNormalizeWorker *worker, u32 loc);

static inline bool eval_normalize_queue_ensure(EvalNormalizeWorker *worker) {
  WsDeque *q = &worker->dq;
  if (q->buf != NULL) {
    return true;
  }
  return wsq_init(q, EVAL_NORMALIZE_WS_CAP_POW2);
}

static inline void eval_normalize_enqueue(EvalNormalizeCtx *ctx, EvalNormalizeWorker *worker, u32 loc) {
  if (__builtin_expect(worker->dq.buf == NULL, 0)) {
    if (!eval_normalize_queue_ensure(worker)) {
      fprintf(stderr, "eval_normalize: queue allocation failed\n");
      exit(1);
    }
  }
  if (!wsq_push(&worker->dq, (u64)loc)) {
    eval_normalize_go(ctx, worker, loc);
  }
}

static inline void eval_normalize_go(EvalNormalizeCtx *ctx, EvalNormalizeWorker *worker, u32 loc) {
  for (;;) {
    if (loc == 0 || !uset_add(&ctx->seen, loc)) {
      return;
    }
    Term term = __builtin_expect(STEPS_ENABLE, 0) ? wnf_steps_at(loc) : wnf_at(loc);
    u32 tloc = term_val(term);
    u8  tag  = term_tag(term);
    // DP0/DP1 have term_arity == 0, handle separately
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
    u64 task;
    if (atomic_load_explicit(&ctx->pending.v, memory_order_acquire) == 0) {
      break;
    }
    if (wsq_pop(&worker->dq, &task)) {
      u32 loc = (u32)task;
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
          u32 loc = (u32)task;
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

fn Term eval_normalize(Term term) {
  wnf_set_tid(0);

  u32 root_loc = (u32)heap_alloc(1);
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
  atomic_store_explicit(&ctx.pending.v, n, memory_order_relaxed);
  for (u32 i = 0; i < n; i++) {
    WsDeque *q = &ctx.W[i].dq;
    q->buf  = NULL;
    q->cap  = 0;
    q->mask = 0;
    atomic_store_explicit(&q->top.v, 0, memory_order_relaxed);
    atomic_store_explicit(&q->bot.v, 0, memory_order_relaxed);
  }
  uset_init(&ctx.seen);

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

  for (u32 i = 0; i < n; i++) {
    wsq_free(&ctx.W[i].dq);
  }
  uset_free(&ctx.seen);

  if (STEPS_ENABLE) {
    STEPS_ROOT_LOC = 0;
  }

  return heap_read(root_loc);
}
