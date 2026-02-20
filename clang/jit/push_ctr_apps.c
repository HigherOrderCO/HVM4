// JIT Module: Constructor Arg Expansion
// -------------------------------------
// Converts constructor fields into APP frames so subsequent LAM states can
// consume them exactly like interpreter APP-MAT-CTR behavior.

// Pushes constructor fields as APP frames so downstream LAM states can consume them.
void jit_push_ctr_apps(Term *stack, u32 *s_pos, Term *heap, Term ctr, u8 ctr_tag) {
  u32 ari = (u32)(ctr_tag - C00);
  if (ari == 0) {
    return;
  }

  u64 ctr_loc = jit_val(ctr);
  for (u32 i = ari; i > 0; i--) {
    u64 app_loc = heap_alloc(2);
    heap_set(app_loc + 0, term_new_era());
    heap_set(app_loc + 1, heap[ctr_loc + (u64)(i - 1)]);
    stack[*s_pos] = term_new(0, APP, 0, app_loc);
    *s_pos = *s_pos + 1;
  }
}
