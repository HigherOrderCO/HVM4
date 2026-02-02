// CNF (collapsed normal form) step.
// - cnf reduces to WNF, then lifts the first SUP to the top.
// - Output is either SUP/ERA/INC at the root with arbitrary fields, or a term
//   with no SUP/ERA/INC at any position.
// - RED keeps only RHS; ERA propagates upward.

#include <stdatomic.h>

#ifndef CNF_POOL_WS_CAP_POW2
#define CNF_POOL_WS_CAP_POW2 16u
#endif
#ifndef CNF_POOL_SPAWN_DEPTH
#define CNF_POOL_SPAWN_DEPTH 8u
#endif

typedef struct {
  Term term;
  u32  depth;
  u32  par_depth;
  Term *out;
  _Atomic u32 *pending;
} CnfTask;

typedef struct {
  u32 n;
  WsDeque dq[MAX_THREADS];
  _Atomic u64 pending;
} CnfPool;

static _Atomic(CnfPool *) CNF_POOL = NULL;

fn Term cnf_at(Term term, u32 depth, u32 par_depth);

fn void cnf_pool_set(CnfPool *pool) {
  atomic_store_explicit(&CNF_POOL, pool, memory_order_release);
}

fn void cnf_pool_clear(void) {
  atomic_store_explicit(&CNF_POOL, NULL, memory_order_release);
}

fn u8 cnf_pool_init(CnfPool *pool, u32 n) {
  pool->n = n;
  atomic_store_explicit(&pool->pending, 0, memory_order_relaxed);
  for (u32 i = 0; i < n; ++i) {
    if (!wsq_init(&pool->dq[i], CNF_POOL_WS_CAP_POW2)) {
      for (u32 j = 0; j < i; ++j) {
        wsq_free(&pool->dq[j]);
      }
      pool->n = 0;
      return 0;
    }
  }
  return 1;
}

fn void cnf_pool_free(CnfPool *pool) {
  for (u32 i = 0; i < pool->n; ++i) {
    wsq_free(&pool->dq[i]);
  }
  pool->n = 0;
  atomic_store_explicit(&pool->pending, 0, memory_order_relaxed);
}

static inline CnfPool *cnf_pool_ctx(void) {
  return atomic_load_explicit(&CNF_POOL, memory_order_acquire);
}

static inline void cnf_task_run_spawned(CnfPool *pool, CnfTask *task) {
  Term res = cnf_at(task->term, task->depth, task->par_depth);
  *task->out = res;
  atomic_fetch_sub_explicit(task->pending, 1, memory_order_release);
  atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_relaxed);
}

static inline void cnf_task_run_inline(CnfTask *task) {
  Term res = cnf_at(task->term, task->depth, task->par_depth);
  *task->out = res;
}

static inline CnfTask *cnf_pool_try_pop(CnfPool *pool, u32 tid) {
  u64 task = 0;
  if (wsq_pop(&pool->dq[tid], &task)) {
    return (CnfTask *)(uintptr_t)task;
  }
  u32 n = pool->n;
  for (u32 k = 1; k < n; ++k) {
    u32 vic = (tid + k) % n;
    if (wsq_steal(&pool->dq[vic], &task)) {
      return (CnfTask *)(uintptr_t)task;
    }
  }
  return NULL;
}

static inline void cnf_pool_join(CnfPool *pool, _Atomic u32 *pending) {
  if (!pool || pool->n <= 1) {
    return;
  }
  if (atomic_load_explicit(pending, memory_order_acquire) == 0) {
    return;
  }
  u32 me = WNF_TID;
  u32 idle = 0;
  while (atomic_load_explicit(pending, memory_order_acquire) != 0) {
    CnfTask *task = cnf_pool_try_pop(pool, me);
    if (task) {
      cnf_task_run_spawned(pool, task);
      idle = 0;
      continue;
    }
    if (idle < 1024) {
      cpu_relax();
      idle++;
    } else {
      sched_yield();
      idle = 0;
    }
  }
}

static inline void cnf_pool_spawn(CnfPool *pool, CnfTask *task) {
  if (!pool || pool->n <= 1) {
    cnf_task_run_inline(task);
    return;
  }
  atomic_fetch_add_explicit(task->pending, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&pool->pending, 1, memory_order_relaxed);
  u32 tid = WNF_TID;
  if (wsq_push(&pool->dq[tid], (u64)(uintptr_t)task)) {
    return;
  }
  atomic_fetch_sub_explicit(task->pending, 1, memory_order_release);
  atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_relaxed);
  cnf_task_run_inline(task);
}

fn u8 cnf_pool_try_run(void) {
  CnfPool *pool = cnf_pool_ctx();
  if (!pool || pool->n <= 1) {
    return 0;
  }
  if (atomic_load_explicit(&pool->pending, memory_order_acquire) == 0) {
    return 0;
  }
  u32 me = WNF_TID;
  CnfTask *task = cnf_pool_try_pop(pool, me);
  if (!task) {
    return 0;
  }
  cnf_task_run_spawned(pool, task);
  return 1;
}

fn Term cnf_at(Term term, u32 depth, u32 par_depth) {
  u32 next_par = par_depth > 0 ? (par_depth - 1u) : 0u;
  term = wnf(term);

  switch (term_tag(term)) {
    case ERA:
    case REF:
    case NUM:
    case NAM:
    case BJV:
    case BJ0:
    case BJ1:
    case BG0:
    case BG1: {
      return term;
    }

    case SUP: {
      return term;
    }

    case INC: {
      return term;
    }

    case RED: {
      u64 loc = term_val(term);
      return cnf_at(heap_read(loc + 1), depth, next_par);
    }

    case LAM: {
      u64  lam_loc = term_val(term);
      Term body    = heap_read(lam_loc);
      u32  level   = depth + 1;
      heap_subst_var(lam_loc, term_new(0, BJV, 0, level));
      Term body_collapsed = cnf_at(body, level, next_par);
      u64  body_loc = heap_alloc(1);
      heap_set(body_loc, body_collapsed);
      Term lam = term_new(0, LAM, level, body_loc);

      u8 body_tag = term_tag(body_collapsed);
      if (body_tag == ERA) {
        return term_new_era();
      }

      if (body_tag == INC) {
        u32 inc_loc = term_val(body_collapsed);
        heap_set(body_loc, heap_read(inc_loc));
        return term_new_inc(lam);
      }

      if (body_tag != SUP) {
        return lam;
      }

      u32  lab     = term_ext(body_collapsed);
      u64  sup_loc = term_val(body_collapsed);
      Term sup_a   = heap_read(sup_loc + 0);
      Term sup_b   = heap_read(sup_loc + 1);

      u64 loc0 = heap_alloc(1);
      u64 loc1 = heap_alloc(1);
      heap_set(loc0, sup_a);
      heap_set(loc1, sup_b);

      Term lam0 = term_new(0, LAM, level, loc0);
      Term lam1 = term_new(0, LAM, level, loc1);

      return term_new_sup(lab, lam0, lam1);
    }

    case DUP:
    case GET:
    case APP:
    case DRY:
    case MAT:
    case SWI:
    case USE:
    case PRI:
    case OP2:
    case DSU:
    case DDU:
    case EQL:
    case AND:
    case OR:
    case UNS:
    case TUP:
    case C00 ... C16: {
      u32 ari = term_arity(term);
      u32 loc = (u32)term_val(term);

      if (ari == 0) {
        return term;
      }

      int  sup_idx = -1;
      Term orig[16];
      Term children[16];
      CnfTask tasks[16];

      CnfPool *pool = cnf_pool_ctx();
      if (pool && pool->n > 1 && ari > 1 && par_depth > 0) {
        _Atomic u32 pending = 0;
        for (u32 i = 0; i < ari; i++) {
          Term child = heap_read(loc + i);
          orig[i] = child;
          tasks[i].term = child;
          tasks[i].depth = depth;
          tasks[i].par_depth = next_par;
          tasks[i].out = &children[i];
          tasks[i].pending = &pending;
          cnf_pool_spawn(pool, &tasks[i]);
        }

        cnf_pool_join(pool, &pending);
        for (u32 i = 0; i < ari; i++) {
          if (children[i] != orig[i]) {
            heap_set(loc + i, children[i]);
          }
          if (term_tag(children[i]) == ERA) {
            return term_new_era();
          }
          if (sup_idx < 0 && term_tag(children[i]) == SUP) {
            sup_idx = (int)i;
          }
        }
      } else {
        for (u32 i = 0; i < ari; i++) {
          Term child = heap_read(loc + i);
          orig[i] = child;
          children[i] = cnf_at(child, depth, next_par);
          if (children[i] != orig[i]) {
            heap_set(loc + i, children[i]);
          }

          if (term_tag(children[i]) == ERA) {
            return term_new_era();
          }

          if (sup_idx < 0 && term_tag(children[i]) == SUP) {
            sup_idx = i;
          }
        }
      }

      if (sup_idx < 0) {
        return term;
      }

      Term sup     = children[sup_idx];
      u32  lab     = term_ext(sup);
      u64  sup_loc = term_val(sup);
      Term sup_a   = heap_read(sup_loc + 0);
      Term sup_b   = heap_read(sup_loc + 1);

      Term args0[16], args1[16];

      for (u32 i = 0; i < ari; i++) {
        if ((int)i == sup_idx) {
          args0[i] = sup_a;
          args1[i] = sup_b;
        } else {
          Copy c = term_clone(lab, children[i]);
          args0[i] = c.k0;
          args1[i] = c.k1;
        }
      }

      Term node0 = term_new_at(loc, term_tag(term), term_ext(term), ari, args0);
      Term node1 = term_new_(term_tag(term), term_ext(term), ari, args1);

      return term_new_sup(lab, node0, node1);
    }

    default: {
      return term;
    }
  }
}

fn Term cnf(Term term) {
  return cnf_at(term, 0, CNF_POOL_SPAWN_DEPTH);
}

fn Term cnf_inj(Term template, Term *args, u32 n_args) {
  if (n_args == 0) {
    return template;
  }

  Term head = wnf(args[0]);

  if (term_tag(head) == SUP) {
    u32  lab     = term_ext(head);
    u64  sup_loc = term_val(head);
    Term sup_a   = heap_read(sup_loc + 0);
    Term sup_b   = heap_read(sup_loc + 1);

    Copy T = term_clone(lab, template);
    Term args0[16], args1[16];
    args0[0] = sup_a;
    args1[0] = sup_b;
    term_clone_many(lab, args + 1, n_args - 1, args0 + 1, args1 + 1);

    Term r0 = cnf_inj(T.k0, args0, n_args);
    Term r1 = cnf_inj(T.k1, args1, n_args);

    return term_new_sup(lab, r0, r1);
  } else {
    Term applied = term_new_app(template, head);
    return cnf_inj(applied, args + 1, n_args - 1);
  }
}
