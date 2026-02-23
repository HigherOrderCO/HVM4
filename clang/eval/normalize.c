// Parallel normalization (SNF) traversal using work-stealing.
#if HVM_WINDOWS
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
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
    if (loc == 0 || !uset_add(&ctx->seen, loc)) {
      return;
    }
    Term term = __builtin_expect(STEPS_ENABLE, 0) ? wnf_steps_at(loc) : wnf_at(loc);
    u64 tloc = term_val(term);
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

#if HVM_WINDOWS
static DWORD WINAPI eval_normalize_worker(LPVOID arg) {
#else
static void *eval_normalize_worker(void *arg) {
#endif
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

#if HVM_WINDOWS
    SwitchToThread();
#else
    sched_yield();
#endif
  }

#if HVM_WINDOWS
  return 0;
#else
  return NULL;
#endif
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
  atomic_store_explicit(&ctx.pending.v, n, memory_order_relaxed);
  for (u32 i = 0; i < n; i++) {
    if (!wsq_init(&ctx.W[i].dq, EVAL_NORMALIZE_WS_CAP_POW2)) {
      fprintf(stderr, "eval_normalize: queue allocation failed\n");
      exit(1);
    }
  }
  uset_init(&ctx.seen);

  eval_normalize_enqueue(&ctx, &ctx.W[0], root_loc);

#if HVM_WINDOWS
  HANDLE tids[MAX_THREADS];
#else
  pthread_t tids[MAX_THREADS];
#endif
  EvalNormalizeArg args[MAX_THREADS];
  for (u32 i = 1; i < n; i++) {
    args[i].ctx = &ctx;
    args[i].tid = i;
#if HVM_WINDOWS
    tids[i] = CreateThread(NULL, 0, eval_normalize_worker, &args[i], 0, NULL);
    if (tids[i] == NULL) {
      fprintf(stderr, "eval_normalize: CreateThread failed\n");
      exit(1);
    }
#else
    pthread_create(&tids[i], NULL, eval_normalize_worker, &args[i]);
#endif
  }

  EvalNormalizeArg arg0 = { .ctx = &ctx, .tid = 0 };
  eval_normalize_worker(&arg0);

  for (u32 i = 1; i < n; i++) {
#if HVM_WINDOWS
    WaitForSingleObject(tids[i], INFINITE);
    CloseHandle(tids[i]);
#else
    pthread_join(tids[i], NULL);
#endif
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
