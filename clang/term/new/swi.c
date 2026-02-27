// SWI: λ{num: f; g} - number switch (same as MAT but for parser/printer)
fn Term term_new_swi(u32 num, Term f, Term g) {
  u64 loc = heap_alloc_kind(2, AOT_HEAP_KIND_TERM_SWI);
  heap_set(loc + 0, f);
  heap_set(loc + 1, g);
  return term_new(0, SWI, num, loc);
}
