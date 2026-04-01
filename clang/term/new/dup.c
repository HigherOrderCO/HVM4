fn Term term_new_dup_at(u64 loc, u32 lab, Term val, Term bod) {
  heap_set(loc + 0, val);
  heap_set(loc + 1, bod);
  return term_new(0, DUP, lab, loc);
}

fn Term term_new_dup(u32 lab, Term val, Term bod) {
  return term_new_dup_at(heap_alloc(2), lab, val, bod);
}
