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

// Per-definition compiled entrypoint table (BOOK id -> native function).
static HvmAotFn AOT_FNS[BOOK_CAP] = {0};

// Rebuilds an ALO node from captured APP-LAM arguments.
fn Term aot_fallback_alo(u64 tm_loc, u16 len, const Term *args) {
  if (len == 0) {
    return term_new(0, ALO, 0, tm_loc);
  }

  u64 ls_loc = 0;
  for (u16 i = 0; i < len; i++) {
    u64 bind = heap_alloc(2);
    heap_set(bind + 0, term_sub_set(args[i], 1));
    heap_set(bind + 1, term_new_num(ls_loc));
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
