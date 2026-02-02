// Parallel normalization (SNF) traversal using work-stealing.
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>

#ifndef EVAL_NORMALIZE_WS_CAP_POW2
#define EVAL_NORMALIZE_WS_CAP_POW2 21
#endif

#define EVAL_NORMALIZE_SEEN_INIT (1u << 20)

typedef struct __attribute__((aligned(64))) {
  WsDeque dq;
  Uset  seen;
} EvalNormalizeWorker;

typedef struct {
  EvalNormalizeWorker   W[MAX_THREADS];
  u32         n;
  _Atomic u64 pending;
} EvalNormalizeCtx;

typedef struct {
  EvalNormalizeCtx *ctx;
  u32 tid;
} EvalNormalizeArg;


static inline void eval_normalize_go(EvalNormalizeCtx *ctx, EvalNormalizeWorker *worker, u32 loc);

static inline void eval_normalize_enqueue(EvalNormalizeCtx *ctx, EvalNormalizeWorker *worker, u32 loc) {
  if (loc == 0) {
    return;
  }
  if (!wsq_push(&worker->dq, (u64)loc)) {
    eval_normalize_go(ctx, worker, loc);
  }
}

static inline void eval_normalize_go(EvalNormalizeCtx *ctx, EvalNormalizeWorker *worker, u32 loc) {
  for (;;) {
    if (!uset_add(&worker->seen, loc)) {
      return;
    }
    Term term = __builtin_expect(STEPS_ENABLE, 0) ? wnf_steps_at(loc) : wnf_at(loc);
    u32 tloc = term_val(term);
    // DP0/DP1 have term_arity == 0, handle separately
    u8  tag  = term_tag(term);
    if (tag == DP0 || tag == DP1 || tag == GT0 || tag == GT1) {
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
  u32 idle = 0;
  bool active = true;

  for (;;) {
    u64 task;
    if (wsq_pop(&worker->dq, &task)) {
      u32 loc = (u32)task;
      eval_normalize_go(ctx, worker, loc);
      continue;
    }
    if (active) {
      atomic_fetch_sub_explicit(&ctx->pending, 1, memory_order_release);
      active = false;
      idle   = 0;
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
      if (wsq_steal(&ctx->W[vic].dq, &task)) {
        stolen = true;
        active = true;
        atomic_fetch_add_explicit(&ctx->pending, 1, memory_order_release);
        u32 loc = (u32)task;
        eval_normalize_go(ctx, worker, loc);
        break;
      }
    }

    if (stolen) {
      continue;
    }

    if (atomic_load_explicit(&ctx->pending, memory_order_acquire) == 0) {
      break;
    }

    if (idle < 1024) {
      cpu_relax();
      idle++;
    } else {
      sched_yield();
      idle = 0;
    }
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
  atomic_store_explicit(&ctx.pending, n, memory_order_relaxed);
  for (u32 i = 0; i < n; i++) {
    if (!wsq_init(&ctx.W[i].dq, EVAL_NORMALIZE_WS_CAP_POW2)) {
      fprintf(stderr, "eval_normalize: queue allocation failed\n");
      exit(1);
    }
    uset_init(&ctx.W[i].seen, EVAL_NORMALIZE_SEEN_INIT);
  }

  EvalNormalizeWorker *worker0 = &ctx.W[0];
  eval_normalize_enqueue(&ctx, worker0, root_loc);

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
    uset_free(&ctx.W[i].seen);
  }

  if (STEPS_ENABLE) {
    STEPS_ROOT_LOC = 0;
  }

  return heap_read(root_loc);
}
