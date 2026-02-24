// Eval collapse (CNF flattening).
// - Lazy CNF extraction via key queue traversal.
// - Lower numeric keys are popped first.
// - INC decreases key (explored earlier), SUP increases key.
// - Uses cnf to handle infinite structures without stack overflow.

#ifndef EVAL_COLLAPSE_STEAL_PERIOD
#define EVAL_COLLAPSE_STEAL_PERIOD 128u
#endif
#ifndef EVAL_COLLAPSE_STEAL_BATCH
#define EVAL_COLLAPSE_STEAL_BATCH 32u
#endif
#ifndef EVAL_COLLAPSE_STACK_SIZE
#define EVAL_COLLAPSE_STACK_SIZE (64u * 1024u * 1024u)
#endif

typedef struct {
  _Alignas(CACHE_L1) CachePaddedAtomic printed;
  _Alignas(CACHE_L1) CachePaddedAtomic pending;
  _Alignas(CACHE_L1) CachePaddedAtomic stop;
  u64  limit;
  u64  print_batch;
  int  silent;
  int  show_itrs;
  Wspq ws;
  CnfPool cnf;
} EvalCollapseCtx;

typedef struct {
  EvalCollapseCtx *ctx;
  u32 tid;
} EvalCollapseArg;

static inline void eval_collapse_process_loc(EvalCollapseCtx *C, u32 me, u8 key, u64 loc, u64 *printed) {
  Term before = heap_read(loc);
  for (;;) {
    if (atomic_load_explicit(&C->stop.v, memory_order_acquire)) {
      return;
    }

    Term t = cnf(before);

    switch (term_tag(t)) {
      case INC: {
        u64 inc_loc = term_val(t);
        before = heap_read(inc_loc);
        if (key > 0) {
          key -= 1;
        }
        continue;
      }
      case SUP: {
        u64 sup_loc = term_val(t);
        u8  nkey = (u8)(key + 1);
        wspq_push(&C->ws, me, nkey, sup_loc + 0);
        wspq_push(&C->ws, me, nkey, sup_loc + 1);
        return;
      }
      case ERA: {
        return;
      }
      default: {
        // Increment the printed counter in batches to avoid bottlenecking when collapsing too many leaves.
        u64 global_printed = atomic_load_explicit(&C->printed.v, memory_order_acquire);
        *printed += 1;
        if (global_printed + *printed >= C->limit && *printed > 0) {
          atomic_store_explicit(&C->stop.v, 1, memory_order_release);
          atomic_fetch_add_explicit(&C->printed.v, *printed, memory_order_release);
          *printed = 0;
        }
        if (*printed >= C->print_batch) {
          atomic_fetch_add_explicit(&C->printed.v, *printed, memory_order_release);
          *printed = 0;
        }
        global_printed = atomic_load_explicit(&C->printed.v, memory_order_acquire);
        if (global_printed + *printed <= C->limit) {
          if (!C->silent) {
            print_term_quoted(t);
            if (C->show_itrs) {
              printf(" \033[2m#%llu\033[0m", (unsigned long long)wnf_itrs_total());
            }
            printf("\n");
          }
        }
        return;
      }
    }
  }
}

static void *eval_collapse_worker(void *arg) {
  EvalCollapseArg *A = (EvalCollapseArg *)arg;
  EvalCollapseCtx *C = A->ctx;
  u32 me = A->tid;

  wnf_set_tid(me);

  u32 iter = 0;
  u32 steal_period = EVAL_COLLAPSE_STEAL_PERIOD;
  u32 steal_batch = EVAL_COLLAPSE_STEAL_BATCH;
  u32 steal_cursor = me + 1;
  u64 limit = C->limit;
  bool active = true;
  u64 local_printed = 0;

  for (;;) {
    if (atomic_load_explicit(&C->stop.v, memory_order_acquire)) {
      break;
    }

    u8  key  = 0;
    u64 task = 0;
    bool popped = wspq_pop(&C->ws, me, &key, &task);
    if (popped) {
      if (task != 0) {
        eval_collapse_process_loc(C, me, key, task, &local_printed);
        iter += 1u;
      }
      if (iter < steal_period) {
        continue;
      }
    } else {
      iter = steal_period;
    }

    u32 stolen = 0;
    if (iter >= steal_period && wspq_can_steal(&C->ws, me)) {
      if (!active) {
        atomic_fetch_add_explicit(&C->pending.v, 1, memory_order_release);
        active = true;
      }
      stolen = wspq_steal_some(&C->ws, me, steal_batch, popped, &steal_cursor);
      iter = 0;
      if (stolen > 0u) {
        continue;
      }
    }

    if (cnf_pool_try_run(me)) {
      continue;
    }

    if (active && !popped && !stolen) {
      atomic_fetch_sub_explicit(&C->pending.v, 1, memory_order_release);
      active = false;
    }

    if (atomic_load_explicit(&C->pending.v, memory_order_acquire) == 0) {
      if (atomic_load_explicit(&C->cnf.pending.v, memory_order_acquire) == 0) {
        atomic_store_explicit(&C->stop.v, 1, memory_order_release);
      }
    }

    if (!active) {
      sched_yield();
    }
  }

  atomic_fetch_add_explicit(&C->printed.v, local_printed, memory_order_relaxed);
  return NULL;
}

fn void eval_collapse(Term term, int limit, int show_itrs, int silent) {
  u32 n = thread_get_count();
  if (limit == 0) {
    return;
  }

  u64 max_lines = UINT64_MAX;
  if (limit >= 0) {
    max_lines = (u64)limit;
  }

  u64 root_loc = heap_alloc(1);
  heap_set(root_loc, term);

  EvalCollapseCtx C;
  atomic_store_explicit(&C.printed.v, 0, memory_order_relaxed);
  atomic_store_explicit(&C.pending.v, n, memory_order_relaxed);
  C.limit = max_lines;

  u64 batch = max_lines / (100 * (u64)n);
  if (batch < 1) batch = 1;
  if (batch > 64) batch = 64;
  C.print_batch = batch;

  C.silent = silent;
  C.show_itrs = show_itrs;
  if (!wspq_init(&C.ws, n)) {
    fprintf(stderr, "eval_collapse: queue allocation failed\n");
    exit(1);
  }
  if (!cnf_pool_init(&C.cnf, n)) {
    fprintf(stderr, "eval_collapse: cnf queue allocation failed\n");
    exit(1);
  }
  cnf_pool_set(&C.cnf);

  wspq_push(&C.ws, 0u, 0u, (u64)root_loc);

  pthread_t tids[MAX_THREADS];
  EvalCollapseArg args[MAX_THREADS];
  if (n > 1) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)EVAL_COLLAPSE_STACK_SIZE);
    for (u32 i = 1; i < n; ++i) {
      args[i].ctx = &C;
      args[i].tid = i;
      pthread_create(&tids[i], &attr, eval_collapse_worker, &args[i]);
    }
    pthread_attr_destroy(&attr);
  }

  EvalCollapseArg arg0 = { .ctx = &C, .tid = 0 };
  eval_collapse_worker(&arg0);

  if (n > 1) {
    for (u32 i = 1; i < n; ++i) {
      pthread_join(tids[i], NULL);
    }
  }

  wspq_free(&C.ws);
  cnf_pool_clear();
  cnf_pool_free(&C.cnf);
}
