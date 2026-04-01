fn Term term_new_lam_at(u64 loc, Term bod) {
  heap_set(loc, bod);
  return term_new(0, LAM, 0, loc);
}

fn Term term_new_lam(Term bod) {
  return term_new_lam_at(heap_alloc(1), bod);
}
