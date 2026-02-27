fn Term term_new_ctr_at(u64 loc, u32 nam, u32 ari, Term *args) {
  return term_new_at(loc, C00 + ari, nam, ari, args);
}

fn Term term_new_ctr(u32 nam, u32 ari, Term *args) {
  return term_new_ctr_at(heap_alloc_kind(ari, AOT_HEAP_KIND_TERM_CTR), nam, ari, args);
}
