fn Term term_new_mat_at(u64 loc, u32 nam, Term val, Term nxt) {
  heap_set(loc + 0, val);
  heap_set(loc + 1, nxt);
  return term_new(0, MAT, nam, loc);
}

fn Term term_new_mat(u32 nam, Term val, Term nxt) {
  return term_new_mat_at(heap_alloc(2), nam, val, nxt);
}
