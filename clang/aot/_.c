// HVM4 AOT Runtime State
// ======================
//
// This module owns runtime state used by ahead-of-time compiled definitions.
//
// Design overview:
// - Trigger modes:
//   - `--to-c` emits a standalone C program to stdout.
//   - `--compile <exe>` emits + compiles with `clang -O2`.
//   - `--jit` emits + compiles + runs once, then removes only the executable.
// - Generated program model:
//   - Includes the real runtime translation unit (`clang/hvm4.c`) directly.
//   - Emits one native C function per top-level definition.
//   - Registers those symbols in `AOT_FNS[id]` before evaluation.
// - Fast path:
//   - Compiled functions partially evaluate WNF for a statically known REF head.
//   - Covered interactions: APP-LAM, APP-MAT-NUM, APP-MAT-CTR.
// - Fallback:
//   - Any unsupported shape immediately rebuilds an ALO `{subterm | env}` from
//     the current static location and captured lambda arguments.
//
// Why this shape:
// - Runtime helpers (term accessors, allocator, heap read/write) are compiled in
//   the same translation unit as generated functions, enabling inlining.
// - There is no shared-library ABI shim and no dlopen/dlsym path in the hot path.

typedef Term (*HvmAotFn)(Term *stack, u32 *s_pos, u32 base);

// Build-time runtime config embedded into generated AOT executables.
typedef struct {
  u32            threads;
  int            debug;
  RuntimeEvalCfg eval;
  u32            ffi_len;
  RuntimeFfiLoad ffi[RUNTIME_FFI_MAX];
} AotBuildCfg;

// Per-definition compiled entrypoint table (BOOK id -> native function).
static HvmAotFn AOT_FNS[BOOK_CAP] = {0};

// Counts one interaction on compiled paths.
fn void aot_itrs_inc(void) {
  if (ITRS_ENABLED) {
    ITRS++;
  }
}

// Adds a known number of interactions on compiled paths.
fn void aot_itrs_add(u64 amount) {
  if (ITRS_ENABLED) {
    ITRS += amount;
  }
}

// Rebuilds an ALO node from captured APP-LAM arguments.
fn Term aot_fallback_alo(u64 tm_loc, u16 len, const Term *args) {
  if (len == 0) {
    return term_new(0, ALO, 0, tm_loc);
  }

  u64 ls_loc = 0;
  // ALO substitutions are addressed by de Bruijn level.
  // Head must be the innermost binder to match wnf_alo_var indexing.
  for (u16 i = 0; i < len; i++) {
    u64 bind = heap_alloc(2);
    heap_set(bind + 0, term_sub_set(args[i], 1));
    // Keep full heap location width; term_new_num(u32) truncates on T>1 slices.
    heap_set(bind + 1, term_new(0, NUM, 0, ls_loc));
    ls_loc = bind;
  }

  u64 alo_loc = heap_alloc(1);
  heap_set(alo_loc, ((ls_loc & ALO_LS_MASK) << ALO_TM_BITS) | (tm_loc & ALO_TM_MASK));
  return term_new(0, ALO, len, alo_loc);
}

// Pushes constructor fields as APP frames for downstream LAM states.
fn void aot_push_ctr_apps(Term *stack, u32 *s_pos, Term ctr, u8 ctr_tag) {
  u32 ari = (u32)(ctr_tag - C00);
  if (ari == 0) {
    return;
  }

  u64 ctr_loc = term_val(ctr);
  for (u32 i = ari; i > 0; i--) {
    u64 app_loc = heap_alloc(2);
    heap_set(app_loc + 0, term_new_era());
    heap_set(app_loc + 1, heap_read(ctr_loc + (u64)(i - 1)));
    stack[*s_pos] = term_new(0, APP, 0, app_loc);
    *s_pos = *s_pos + 1;
  }
}

// Hot Eval
// ========
// Executes a larger compiled subset (DUP/OP2/REF recursion) before deopting.

#define AOT_HOT_ENV_CAP 16
#define AOT_HOT_ARG_CAP 16
#define AOT_HOT_MAX_DEPTH 4096

typedef struct {
  Term *data;
  u16   len;
  u16   cap;
} AotHotEnv;

typedef struct {
  u8   ok;
  Term term;
} AotHotRes;

typedef AotHotRes (*HvmAotHotFn)(u16 argc, const Term *args, u32 depth);
typedef struct {
  u8  ok;
  u32 val;
} AotNumRes;
typedef AotNumRes (*HvmAotNumFn)(u16 argc, const u32 *args, u32 depth);

// Per-definition compiled hot-apply table (BOOK id -> native hot call).
static HvmAotHotFn AOT_HOT_FNS[BOOK_CAP] = {0};
static HvmAotNumFn AOT_NUM_FNS[BOOK_CAP] = {0};

// Forward declarations for the recursive hot evaluator.
fn Term      wnf_op2_num_num_raw(u32 opr, u32 a, u32 b);
fn AotHotRes aot_hot_eval_loc(u64 loc, AotHotEnv *env, u32 depth);
fn AotHotRes aot_hot_apply_ref(u16 ref_id, u16 argc, const Term *args, u32 depth);

// Constructs a successful hot-eval result.
fn AotHotRes aot_hot_ok(Term term) {
  return (AotHotRes){ .ok = 1, .term = term };
}

// Constructs a failed hot-eval result with a residual runtime term.
fn AotHotRes aot_hot_fail(Term term) {
  return (AotHotRes){ .ok = 0, .term = term };
}

// Constructs a successful numeric result.
fn AotNumRes aot_num_ok(u32 val) {
  return (AotNumRes){ .ok = 1, .val = val };
}

// Constructs a failed numeric result.
fn AotNumRes aot_num_fail(void) {
  return (AotNumRes){ .ok = 0, .val = 0 };
}

// Applies a compiled numeric REF.
fn AotNumRes aot_num_apply_ref(u16 ref_id, u16 argc, const u32 *args, u32 depth) {
  if (depth >= AOT_HOT_MAX_DEPTH) {
    return aot_num_fail();
  }

  HvmAotNumFn num = AOT_NUM_FNS[ref_id];
  if (num == NULL) {
    return aot_num_fail();
  }

  return num(argc, args, depth);
}

// Converts current hot environment to an ALO fallback at `loc`.
fn Term aot_hot_env_alo(u64 loc, const AotHotEnv *env) {
  if (env->len == 0) {
    return aot_fallback_alo(loc, 0, NULL);
  }
  return aot_fallback_alo(loc, env->len, env->data);
}

// Builds a failed result by deopting current location + environment.
fn AotHotRes aot_hot_fail_loc(u64 loc, const AotHotEnv *env) {
  return aot_hot_fail(aot_hot_env_alo(loc, env));
}

// Reapplies arguments [from, argc) to a head term.
fn Term aot_hot_reapply(Term head, u16 argc, const Term *args, u16 from) {
  for (u16 i = from; i < argc; i++) {
    head = term_new_app(head, args[i]);
  }
  return head;
}

// Deopts call application from current callee location + captured env.
fn AotHotRes aot_hot_fail_apply(u64 loc, const AotHotEnv *env, u16 argc, const Term *args, u16 from) {
  Term head = aot_hot_env_alo(loc, env);
  Term appd = aot_hot_reapply(head, argc, args, from);
  return aot_hot_fail(appd);
}

// Deopts one location with a raw environment vector.
fn AotHotRes aot_hot_fail_loc_env(u64 loc, u16 env_len, const Term *env) {
  Term head = aot_fallback_alo(loc, env_len, env_len == 0 ? NULL : env);
  return aot_hot_fail(head);
}

// Deopts one application with a raw environment vector.
fn AotHotRes aot_hot_fail_apply_env(u64 loc, u16 env_len, const Term *env, u16 argc, const Term *args, u16 from) {
  Term head = aot_fallback_alo(loc, env_len, env_len == 0 ? NULL : env);
  Term appd = aot_hot_reapply(head, argc, args, from);
  return aot_hot_fail(appd);
}

// Evaluates one location from a raw mutable environment vector.
fn AotHotRes aot_hot_eval_loc_env(u64 loc, Term *env_data, u16 env_len, u16 env_cap, u32 depth) {
  if (env_len > env_cap) {
    return aot_hot_fail_loc_env(loc, env_len, env_data);
  }
  AotHotEnv env = {
    .data = env_data,
    .len  = env_len,
    .cap  = env_cap,
  };
  return aot_hot_eval_loc(loc, &env, depth);
}

// Returns 1 when DUP can be handled as a no-op copy in hot path.
fn int aot_hot_is_copy_free(Term term) {
  u8 tag = term_tag(term);
  if (tag == NUM) {
    return 1;
  }
  if (tag == C00) {
    return 1;
  }
  return 0;
}

// Looks up one de Bruijn level from current hot environment.
fn int aot_hot_lookup(const AotHotEnv *env, u16 lvl, u8 tag, Term *out) {
  if (lvl == 0 || lvl > env->len) {
    return 0;
  }

  u16 idx = env->len - lvl;
  switch (tag) {
    case VAR:
    case BJV: {
      *out = env->data[idx];
      return 1;
    }
    case DP0:
    case BJ0: {
      *out = env->data[idx];
      return 1;
    }
    case DP1:
    case BJ1: {
      *out = env->data[idx];
      return 1;
    }
    default: {
      return 0;
    }
  }
}

// Evaluates one OP2 node when both sides remain inside compiled subset.
fn AotHotRes aot_hot_eval_op2(u64 loc, Term op2, AotHotEnv *env, u32 depth) {
  u16 opr = term_ext(op2);
  u64 arg = term_val(op2);

  AotHotRes lhs = aot_hot_eval_loc(arg + 0, env, depth);
  if (!lhs.ok) {
    Term rhs = aot_hot_env_alo(arg + 1, env);
    Term res = term_new_op2(opr, lhs.term, rhs);
    return aot_hot_fail(res);
  }
  if (term_tag(lhs.term) != NUM) {
    return aot_hot_fail_loc(loc, env);
  }

  AotHotRes rhs = aot_hot_eval_loc(arg + 1, env, depth);
  if (!rhs.ok) {
    Term res = term_new_op2(opr, lhs.term, rhs.term);
    return aot_hot_fail(res);
  }
  if (term_tag(rhs.term) != NUM) {
    Term res = term_new_op2(opr, lhs.term, rhs.term);
    return aot_hot_fail(res);
  }

  Term out = wnf_op2_num_num_raw(opr, (u32)term_val(lhs.term), (u32)term_val(rhs.term));
  return aot_hot_ok(out);
}

// Evaluates one DUP node with a copy-free duplicated value.
fn AotHotRes aot_hot_eval_dup(u64 loc, Term dup, AotHotEnv *env, u32 depth) {
  u64 dup_loc = term_val(dup);

  AotHotRes val = aot_hot_eval_loc(dup_loc + 0, env, depth);
  if (!val.ok) {
    return aot_hot_fail_loc(loc, env);
  }
  if (!aot_hot_is_copy_free(val.term)) {
    return aot_hot_fail_loc(loc, env);
  }
  if (env->len >= env->cap) {
    return aot_hot_fail_loc(loc, env);
  }

  env->data[env->len++] = val.term;
  aot_itrs_inc();
  AotHotRes out = aot_hot_eval_loc(dup_loc + 1, env, depth);
  env->len--;
  return out;
}

// Evaluates a static APP chain whose head is a REF using compiled recursion.
fn AotHotRes aot_hot_eval_app(u64 loc, AotHotEnv *env, u32 depth) {
  u64 head_loc = loc;
  u64 arg_locs[AOT_HOT_ARG_CAP];
  u16 arg_len = 0;

  for (;;) {
    Term head = heap_read(head_loc);
    if (term_tag(head) != APP) {
      break;
    }
    if (arg_len >= AOT_HOT_ARG_CAP) {
      return aot_hot_fail_loc(loc, env);
    }
    u64 app_loc = term_val(head);
    arg_locs[arg_len++] = app_loc + 1;
    head_loc = app_loc + 0;
  }

  Term head = heap_read(head_loc);
  if (term_tag(head) != REF) {
    return aot_hot_fail_loc(loc, env);
  }

  Term args[AOT_HOT_ARG_CAP];
  for (u16 i = 0; i < arg_len; i++) {
    u64 arg_loc = arg_locs[arg_len - 1 - i];
    AotHotRes arg = aot_hot_eval_loc(arg_loc, env, depth);
    if (!arg.ok) {
      Term call = term_new_ref(term_ext(head));
      for (u16 j = 0; j < i; j++) {
        call = term_new_app(call, args[j]);
      }
      call = term_new_app(call, arg.term);
      for (u16 j = i + 1; j < arg_len; j++) {
        u64 rem_loc = arg_locs[arg_len - 1 - j];
        call = term_new_app(call, aot_hot_env_alo(rem_loc, env));
      }
      return aot_hot_fail(call);
    }
    args[i] = arg.term;
  }

  return aot_hot_apply_ref(term_ext(head), arg_len, args, depth + 1);
}

// Applies a compiled REF to dynamic args using the same hot evaluator.
fn AotHotRes aot_hot_apply_ref(u16 ref_id, u16 argc, const Term *args, u32 depth) {
  if (depth >= AOT_HOT_MAX_DEPTH) {
    Term call = aot_hot_reapply(term_new_ref(ref_id), argc, args, 0);
    return aot_hot_fail(call);
  }

  HvmAotHotFn hot = AOT_HOT_FNS[ref_id];
  if (hot != NULL) {
    return hot(argc, args, depth);
  }

  if (BOOK[ref_id] == 0) {
    Term call = aot_hot_reapply(term_new_ref(ref_id), argc, args, 0);
    return aot_hot_fail(call);
  }

  Term env_data[AOT_HOT_ENV_CAP];
  AotHotEnv env = {
    .data = env_data,
    .len  = 0,
    .cap  = AOT_HOT_ENV_CAP,
  };
  u64 at  = BOOK[ref_id];
  u16 i   = 0;

  while (i < argc) {
    Term cur = heap_read(at);
    switch (term_tag(cur)) {
      case LAM: {
        if (env.len >= env.cap) {
          return aot_hot_fail_apply(at, &env, argc, args, i);
        }
        env.data[env.len++] = args[i];
        i++;
        aot_itrs_inc();
        at = term_val(cur);
        continue;
      }
      case SWI: {
        Term arg = args[i];
        if (term_tag(arg) != NUM) {
          return aot_hot_fail_apply(at, &env, argc, args, i);
        }
        u64 mat_loc = term_val(cur);
        if (term_val(arg) == term_ext(cur)) {
          aot_itrs_inc();
          i++;
          at = mat_loc + 0;
        } else {
          aot_itrs_inc();
          at = mat_loc + 1;
        }
        continue;
      }
      default: {
        return aot_hot_fail_apply(at, &env, argc, args, i);
      }
    }
  }

  return aot_hot_eval_loc(at, &env, depth);
}

// Evaluates one static location in the hot compiled subset.
fn AotHotRes aot_hot_eval_loc(u64 loc, AotHotEnv *env, u32 depth) {
  if (depth >= AOT_HOT_MAX_DEPTH) {
    return aot_hot_fail_loc(loc, env);
  }

  Term cur = heap_read(loc);
  u8   tag = term_tag(cur);

  switch (tag) {
    case NUM:
    case NAM:
    case ERA:
    case ANY:
    case C00: {
      return aot_hot_ok(cur);
    }
    case REF: {
      return aot_hot_ok(cur);
    }
    case VAR:
    case BJV:
    case DP0:
    case BJ0:
    case DP1:
    case BJ1: {
      Term out;
      if (aot_hot_lookup(env, (u16)term_val(cur), tag, &out)) {
        return aot_hot_ok(out);
      }
      return aot_hot_fail_loc(loc, env);
    }
    case OP2: {
      return aot_hot_eval_op2(loc, cur, env, depth);
    }
    case DUP: {
      return aot_hot_eval_dup(loc, cur, env, depth);
    }
    case APP: {
      return aot_hot_eval_app(loc, env, depth);
    }
    default: {
      return aot_hot_fail_loc(loc, env);
    }
  }
}

// Continues execution from `loc` with captured environment and current WNF stack.
fn Term aot_exec_loc(u64 loc, const Term *env0, u16 env0_len, Term *stack, u32 *s_pos, u32 base) {
  if (env0_len > AOT_HOT_ENV_CAP) {
    return aot_fallback_alo(loc, env0_len, env0);
  }

  Term env_data[AOT_HOT_ENV_CAP];
  AotHotEnv env = {
    .data = env_data,
    .len  = env0_len,
    .cap  = AOT_HOT_ENV_CAP,
  };
  for (u16 i = 0; i < env0_len; i++) {
    env.data[i] = env0[i];
  }

  u64 at = loc;
  while (*s_pos > base) {
    Term frame = stack[*s_pos - 1];
    if (term_tag(frame) != APP) {
      break;
    }

    Term cur = heap_read(at);
    switch (term_tag(cur)) {
      case LAM: {
        if (env.len >= env.cap) {
          return aot_hot_env_alo(at, &env);
        }
        (*s_pos)--;
        u64 app_loc = term_val(frame);
        Term arg    = heap_read(app_loc + 1);
        env.data[env.len++] = arg;
        aot_itrs_inc();
        at = term_val(cur);
        continue;
      }
      case SWI: {
        u64 app_loc = term_val(frame);
        Term arg    = heap_read(app_loc + 1);
        if (term_tag(arg) != NUM) {
          return aot_hot_env_alo(at, &env);
        }
        u64 mat_loc = term_val(cur);
        if (term_val(arg) == term_ext(cur)) {
          (*s_pos)--;
          aot_itrs_inc();
          at = mat_loc + 0;
        } else {
          aot_itrs_inc();
          at = mat_loc + 1;
        }
        continue;
      }
      default: {
        return aot_hot_env_alo(at, &env);
      }
    }
  }

  AotHotRes out = aot_hot_eval_loc(at, &env, 0);
  return out.term;
}
