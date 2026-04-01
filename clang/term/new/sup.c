fn Term term_new_sup_at(u64 loc, u32 lab, Term tm0, Term tm1) {
  heap_set(loc + 0, tm0);
  heap_set(loc + 1, tm1);
  return term_new(0, SUP, lab, loc);
}

fn Term term_new_sup(u32 lab, Term tm0, Term tm1) {
  return term_new_sup_at(heap_alloc(2), lab, tm0, tm1);
}
