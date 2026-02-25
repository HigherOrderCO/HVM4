// HVM AOT Runtime Core
// ====================
//
// This module exposes the tiny runtime surface used by generated AOT code.
//
// Model:
// - Each compiled definition is one stack-entry function: `F_<name>`.
// - Generated code is direct tree-shaped C with explicit fallback points.
// - Deopt returns residual runtime terms (ALO/app chains), never rewinds state.

// Type
// ----

typedef Term (*HvmAotFn)(Term *stack, u32 *s_pos, u32 base);
fn Term wnf(Term term);

// Build-time runtime config embedded into generated AOT executables.
typedef struct {
  u32            threads;
  int            debug;
  RuntimeEvalCfg eval;
  u32            ffi_len;
  RuntimeFfiLoad ffi[RUNTIME_FFI_MAX];
} AotBuildCfg;

// Runtime
// -------

// Per-definition compiled entrypoint table (BOOK id -> native function).
static HvmAotFn AOT_FNS[BOOK_CAP] = {0};

// AOT runtime limits.
#define AOT_ARG_CAP   64
#define AOT_ENV_CAP   64
#define AOT_MAX_DEPTH 4096

// Thread-local compiled-call depth for recursion cutoff.
static _Thread_local u32 AOT_CALL_DEPTH = 0;

// Optional AOT profiling counters (enabled by HVM_AOT_PROFILE=1).
static _Atomic u64 AOT_PROF_FN_ENTRIES   = 0;
static _Atomic u64 AOT_PROF_TRY_CALLS    = 0;
static _Atomic u64 AOT_PROF_TRY_HITS     = 0;
static _Atomic u64 AOT_PROF_FALLBACK_ALO = 0;
static _Atomic u64 AOT_PROF_FORCE_WNF    = 0;
static _Atomic u64 AOT_PROF_HEAD_RETURNS = 0;
static _Atomic u64 AOT_PROF_ITRS         = 0;
static int AOT_PROF_ENABLED = -1;

// Returns whether profiling is enabled for this process.
fn int aot_prof_enabled(void) {
  if (AOT_PROF_ENABLED >= 0) {
    return AOT_PROF_ENABLED;
  }
  const char *env = getenv("HVM_AOT_PROFILE");
  AOT_PROF_ENABLED = env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
  return AOT_PROF_ENABLED;
}

// Increments one profiling counter when enabled.
fn void aot_prof_inc(_Atomic u64 *slot, u64 add) {
  if (aot_prof_enabled()) {
    atomic_fetch_add_explicit(slot, add, memory_order_relaxed);
  }
}

// Counts one interaction on compiled paths.
fn void aot_itrs_inc(void) {
  if (ITRS_ENABLED) {
    ITRS++;
  }
  aot_prof_inc(&AOT_PROF_ITRS, 1);
}

// Adds a known number of interactions on compiled paths.
fn void aot_itrs_add(u64 amount) {
  if (ITRS_ENABLED) {
    ITRS += amount;
  }
  aot_prof_inc(&AOT_PROF_ITRS, amount);
}

// Fallback
// --------

// Rebuilds an ALO node from captured lexical environment entries.
// - Non-dup binders store term_sub_set(value, 1) (LAM semantics).
// - Dup binders reuse the shared DUP cell location stored in args[i].
fn Term aot_fallback_alo(u64 tm_loc, u16 len, const Term *args, u64 env_is_dup) {
  aot_prof_inc(&AOT_PROF_FALLBACK_ALO, 1);
  if (len == 0) {
    return term_new(0, ALO, 0, tm_loc);
  }

  // Keep one contiguous allocation for all freshly built bind entries plus ALO pair.
  // Dup binders may reuse existing cells, so this may reserve extra space.
  u64 block = heap_alloc((u64)len * 2 + 1);
  u64 ls_loc = 0;
  for (u16 i = 0; i < len; i++) {
    u64 bind = block + (u64)i * 2;
    if (env_is_dup & (1ULL << i)) {
      bind = (u64)args[i];
    } else {
      heap_set(bind + 0, term_sub_set(args[i], 1));
    }
    heap_set(bind + 1, term_new(0, NUM, 0, ls_loc));
    ls_loc = bind;
  }

  u64 alo_loc = block + (u64)len * 2;
  u64 alo_val = ((ls_loc & ALO_LS_MASK) << ALO_TM_BITS) | (tm_loc & ALO_TM_MASK);
  heap_set(alo_loc, alo_val);
  return term_new(0, ALO, len, alo_loc);
}

// Reapplies arguments [from, argc) to a head term.
fn Term aot_reapply(Term head, u16 argc, const Term *args, u16 from) {
  for (u16 i = from; i < argc; i++) {
    head = term_new_app(head, args[i]);
  }
  return head;
}

// Returns one copy-free DUP value predicate used by generated code.
fn int aot_is_copy_free(Term term) {
  u8 tag = term_tag(term);
  if (tag == NUM) {
    return 1;
  }
  if (tag == C00) {
    return 1;
  }
  return 0;
}

// Forces one term to WHNF while preserving current stack cursor consistency.
fn Term aot_force_whnf(Term term, Term *stack, u32 *s_pos) {
  (void)stack;
  aot_prof_inc(&AOT_PROF_FORCE_WNF, 1);
  WNF_S_POS = *s_pos;
  Term out = wnf(term);
  *s_pos = WNF_S_POS;
  return out;
}

// Profiles one compiled function entry.
fn void aot_prof_fn_entry(void) {
  aot_prof_inc(&AOT_PROF_FN_ENTRIES, 1);
}

// Profiles one early head return/deopt from compiled code.
fn void aot_prof_head_return(void) {
  aot_prof_inc(&AOT_PROF_HEAD_RETURNS, 1);
}

// Calls
// -----

// Returns current compiled recursion depth.
fn u32 aot_call_depth(void) {
  return AOT_CALL_DEPTH;
}

// Calls one compiled ref using current stack slice, else returns residual REF application.
fn Term aot_call_ref(u32 ref_id, Term *stack, u32 *s_pos, u32 base) {
  if (AOT_CALL_DEPTH >= AOT_MAX_DEPTH) {
    return term_new_ref(ref_id);
  }

  HvmAotFn fun = AOT_FNS[ref_id];
  if (fun == NULL) {
    return term_new_ref(ref_id);
  }

  WNF_S_POS = *s_pos;
  AOT_CALL_DEPTH++;
  Term out = fun(stack, s_pos, base);
  AOT_CALL_DEPTH--;
  WNF_S_POS = *s_pos;

  return out;
}

// Dispatch
// --------

// Tries to execute a compiled function for a REF; returns 0 when absent.
fn int aot_try_call(u32 id, Term *stack, u32 *s_pos, u32 base, Term *out) {
  aot_prof_inc(&AOT_PROF_TRY_CALLS, 1);
  if (STEPS_ITRS_LIM != 0) {
    return 0;
  }

  if (id >= BOOK_CAP) {
    return 0;
  }

  HvmAotFn fun = AOT_FNS[id];
  if (fun == NULL) {
    return 0;
  }
  if (AOT_CALL_DEPTH >= AOT_MAX_DEPTH) {
    return 0;
  }
  WNF_S_POS = *s_pos;
  AOT_CALL_DEPTH++;
  *out = fun(stack, s_pos, base);
  AOT_CALL_DEPTH--;
  WNF_S_POS = *s_pos;
  aot_prof_inc(&AOT_PROF_TRY_HITS, 1);
  return 1;
}

// Prints one profiling summary (stderr) when enabled.
fn void aot_profile_print(u64 total_itrs) {
  if (!aot_prof_enabled()) {
    return;
  }

  u64 entries   = atomic_load_explicit(&AOT_PROF_FN_ENTRIES, memory_order_relaxed);
  u64 tries     = atomic_load_explicit(&AOT_PROF_TRY_CALLS, memory_order_relaxed);
  u64 hits      = atomic_load_explicit(&AOT_PROF_TRY_HITS, memory_order_relaxed);
  u64 fallbacks = atomic_load_explicit(&AOT_PROF_FALLBACK_ALO, memory_order_relaxed);
  u64 forced    = atomic_load_explicit(&AOT_PROF_FORCE_WNF, memory_order_relaxed);
  u64 heads     = atomic_load_explicit(&AOT_PROF_HEAD_RETURNS, memory_order_relaxed);
  u64 aot_itrs  = atomic_load_explicit(&AOT_PROF_ITRS, memory_order_relaxed);

  double hit_rate      = tries == 0 ? 0.0 : (100.0 * (double)hits) / (double)tries;
  double compiled_share = total_itrs == 0 ? 0.0 : (100.0 * (double)aot_itrs) / (double)total_itrs;

  fprintf(stderr, "AOT_PROFILE calls=%llu hits=%llu hit_rate=%.2f%% entries=%llu\n",
          (unsigned long long)tries, (unsigned long long)hits, hit_rate, (unsigned long long)entries);
  fprintf(stderr, "AOT_PROFILE compiled_itrs=%llu total_itrs=%llu compiled_share=%.2f%%\n",
          (unsigned long long)aot_itrs, (unsigned long long)total_itrs, compiled_share);
  fprintf(stderr, "AOT_PROFILE fallback_alo=%llu head_returns=%llu force_wnf=%llu\n",
          (unsigned long long)fallbacks, (unsigned long long)heads, (unsigned long long)forced);
}

// Utils
// -----

// Converts a symbol name into a filesystem-safe alnum/_ identifier.
fn char *aot_sanitize(const char *name) {
  size_t len = strlen(name);
  size_t cap = (len * 4) + 1;
  char *out  = malloc(cap);
  if (out == NULL) {
    return NULL;
  }

  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    u8 c = (u8)name[i];
    if (isalnum(c) || c == '_') {
      out[j++] = (char)c;
      continue;
    }

    if (j + 4 >= cap) {
      free(out);
      return NULL;
    }

    static const char HEX[] = "0123456789ABCDEF";
    out[j++] = '_';
    out[j++] = 'x';
    out[j++] = HEX[(c >> 4) & 0xF];
    out[j++] = HEX[c & 0xF];
  }

  out[j] = '\0';
  return out;
}
