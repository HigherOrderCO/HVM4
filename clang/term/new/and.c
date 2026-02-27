fn Term term_new_and_at(u64 loc, Term a, Term b) {
  heap_set(loc + 0, a);
  heap_set(loc + 1, b);
  return term_new(0, AND, 0, loc);
}

fn Term term_new_and(Term a, Term b) {
  return term_new_and_at(heap_alloc_kind(2, AOT_HEAP_KIND_TERM_AND), a, b);
}
