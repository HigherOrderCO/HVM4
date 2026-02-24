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
#define AOT_ARG_CAP   16
#define AOT_ENV_CAP   16
#define AOT_MAX_DEPTH 4096

// Thread-local compiled-call depth for recursion cutoff.
static _Thread_local u32 AOT_CALL_DEPTH = 0;

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

// Fallback
// --------

// Rebuilds one plain ALO environment (all binders substituted).
fn Term aot_fallback_alo(u64 tm_loc, u16 len, const Term *args) {
  if (len == 0) {
    return term_new(0, ALO, 0, tm_loc);
  }

  u64 block  = heap_alloc((u64)len * 2 + 1);
  u64 ls_loc = 0;
  for (u16 i = 0; i < len; i++) {
    u64 bind = block + (u64)i * 2;
    heap_set(bind + 0, term_sub_set(args[i], 1));
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
fn int aot_is_copy_free_static_rec(Term term, u32 fuel) {
  if (fuel == 0) {
    return 0;
  }

  u8 tag = term_tag(term);
  if (tag == NUM || tag == C00) {
    return 1;
  }
  if (tag < C01 || tag > C16) {
    return 0;
  }

  u64 loc = term_val(term);
  if (loc >= HEAP_STATIC_END) {
    return 0;
  }

  u32 ari = term_arity(term);
  for (u32 i = 0; i < ari; i++) {
    Term fld = heap_read(loc + i);
    if (!aot_is_copy_free_static_rec(fld, fuel - 1)) {
      return 0;
    }
  }
  return 1;
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
  if (tag >= C01 && tag <= C16) {
    u64 loc = term_val(term);
    if (loc < HEAP_STATIC_END) {
      return aot_is_copy_free_static_rec(term, 1024);
    }
  }
  return 0;
}

// Calls
// -----

// Returns current compiled recursion depth.
fn u32 aot_call_depth(void) {
  return AOT_CALL_DEPTH;
}

// Calls one compiled ref if available, else returns residual REF application.
fn Term aot_call_ref_direct(HvmAotFn fun, u32 ref_id, u16 argc, const Term *args) {
  if (argc > AOT_ARG_CAP) {
    return aot_reapply(term_new_ref(ref_id), argc, args, 0);
  }

  if (AOT_CALL_DEPTH >= AOT_MAX_DEPTH) {
    return aot_reapply(term_new_ref(ref_id), argc, args, 0);
  }

  if (fun == NULL) {
    return aot_reapply(term_new_ref(ref_id), argc, args, 0);
  }

  Term stack[AOT_ARG_CAP];
  u32  s_pos  = argc;
  u64  block  = heap_alloc((u64)argc * 2);
  for (u16 i = 0; i < argc; i++) {
    u64 app_loc = block + (u64)i * 2;
    heap_set(app_loc + 0, term_new_era());
    heap_set(app_loc + 1, args[i]);
    stack[argc - 1 - i] = term_new(0, APP, 0, app_loc);
  }

  AOT_CALL_DEPTH++;
  Term out = fun(stack, &s_pos, 0);
  AOT_CALL_DEPTH--;

  while (s_pos > 0) {
    Term frame = stack[--s_pos];
    if (term_tag(frame) != APP) {
      continue;
    }
    u64 app_loc = term_val(frame);
    Term arg = heap_read(app_loc + 1);
    out = term_new_app(out, arg);
  }

  return out;
}

// Calls one compiled ref by id if available, else returns residual REF application.
fn Term aot_call_ref(u32 ref_id, u16 argc, const Term *args) {
  if (ref_id >= BOOK_CAP) {
    return aot_reapply(term_new_ref(ref_id), argc, args, 0);
  }
  return aot_call_ref_direct(AOT_FNS[ref_id], ref_id, argc, args);
}

// Calls one compiled ref directly on the current stack range [base, *s_pos).
// Returns 1 on success and writes the reduced term to `out`.
fn int aot_call_ref_fast(u32 ref_id, Term *stack, u32 *s_pos, u32 base, Term *out) {
  if (ref_id >= BOOK_CAP) {
    return 0;
  }

  HvmAotFn fun = AOT_FNS[ref_id];
  if (fun == NULL) {
    return 0;
  }
  if (AOT_CALL_DEPTH >= AOT_MAX_DEPTH) {
    return 0;
  }
  AOT_CALL_DEPTH++;
  *out = fun(stack, s_pos, base);
  AOT_CALL_DEPTH--;
  return 1;
}

// Dispatch
// --------

// Tries to execute a compiled function for a REF; returns 0 when absent.
fn int aot_try_call(u32 id, Term *stack, u32 *s_pos, u32 base, Term *out) {
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

  *out = fun(stack, s_pos, base);
  return 1;
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
