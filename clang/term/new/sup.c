fn Term term_new_sup_at(u64 loc, u32 lab, Term tm0, Term tm1) {
  return term_new_at(loc, SUP, lab, 2, (Term[]){tm0, tm1});
}

fn Term term_new_sup(u32 lab, Term tm0, Term tm1) {
  return term_new_sup_at(heap_alloc_kind(2, AOT_HEAP_KIND_TERM_SUP), lab, tm0, tm1);
}
