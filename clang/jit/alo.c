// JIT Module: ALO Fallback Builder
// --------------------------------
// Reconstructs an ALO term from the current static location plus captured
// APP-LAM arguments, matching interpreter fallback semantics.

// Rebuilds an ALO node with captured lambda arguments for generic fallback.
Term jit_alo(u64 tm_loc, u16 len, const Term *args) {
  if (len == 0) {
    return term_new(0, ALO, 0, tm_loc);
  }

  u64 ls_loc = 0;
  for (u16 i = 0; i < len; i++) {
    u64 bind_ent = heap_alloc(2);
    heap_set(bind_ent + 0, term_sub_set(args[i], 1));
    heap_set(bind_ent + 1, term_new_num(ls_loc));
    ls_loc = bind_ent;
  }

  u64 alo_loc = heap_alloc(1);
  heap_set(alo_loc, ((ls_loc & ALO_LS_MASK) << ALO_TM_BITS) | (tm_loc & ALO_TM_MASK));
  return term_new(0, ALO, len, alo_loc);
}
