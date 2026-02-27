 fn Term term_new_lam_at(u64 loc, Term bod) {
  return term_new_at(loc, LAM, 0, 1, (Term[]){bod});
}

fn Term term_new_lam(Term bod) {
  return term_new_lam_at(heap_alloc_kind(1, AOT_HEAP_KIND_TERM_LAM), bod);
}
