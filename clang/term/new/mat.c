fn Term term_new_mat_at(u64 loc, u32 nam, Term val, Term nxt) {
  return term_new_at(loc, MAT, nam, 2, (Term[]){val, nxt});
}

fn Term term_new_mat(u32 nam, Term val, Term nxt) {
  return term_new_mat_at(heap_alloc_kind(2, AOT_HEAP_KIND_TERM_MAT), nam, val, nxt);
}
