fn Term term_new_dup_at(u64 loc, u32 lab, Term val, Term bod) {
  return term_new_at(loc, DUP, lab, 2, (Term[]){val, bod});
}

fn Term term_new_dup(u32 lab, Term val, Term bod) {
  return term_new_dup_at(heap_alloc_kind(2, AOT_HEAP_KIND_TERM_DUP), lab, val, bod);
}
